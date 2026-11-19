//! The relay's endpoints as the launcher calls them. The contract (every path, request and answer) is written out in
//! docs/kiln/launcher/discord.md; this file is its client.
//!
//! - One base address ([`super::settings::RelayBase`], default `https://frostdev.io/kiln/relay`), reached through the
//!   [`crate::net`] layer: https only, that one host, no redirects, public addresses only, bounded answers, no URL query
//!   or header value in any error. The session key goes in `Authorization: Bearer <key>` and only to that host.
//! - DELETE is allowed for this client only ([`crate::net::Client::allow_delete`]): sign out, forget, a post the player
//!   deletes, stop testing.
//! - Every refusal becomes a [`crate::Error`] whose `detail.kind` the UI shows ([`Kind`]); a dropped connection is
//!   `offline`, a server that answers badly is `relay`.
//! - Ids from answers are checked before they go into a path; texts and links before they are kept or shown.

use super::settings::RelayBase;
use super::{Account, Feed, SupportPost};
use crate::net::{self, Body, Method, Request, Transport};
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

/// A JSON answer may be this long (posts, the feed: a few KiB).
pub const ANSWER_MAX: u64 = 256 * 1024;
/// The known-problems list may be this long.
pub const KNOWN_MAX: u64 = 512 * 1024;
/// The zip the relay takes (PLAN 7.3).
pub const ZIP_MAX: u64 = 8 * 1024 * 1024;

/// What went wrong, as the UI words it (`detail.kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// No answer: the network is down or frostdev.io can't be reached.
    Offline,
    /// frostdev.io answered, but not as the relay (5xx, nonsense, a refusal without reason).
    Relay,
    /// The session is unknown or ended: sign in again.
    Signin,
    /// Not in the Kiln Discord.
    NotMember,
    /// The Discord account is younger than three days.
    TooYoung,
    /// Too many reports.
    RateLimited,
    /// The zip is over the relay's limit.
    TooBig,
    /// A sign-in that ran out, or a post that is gone.
    Expired,
    NotFound,
    BadRequest,
}

impl Kind {
    pub fn as_str(self) -> &'static str {
        match self {
            Kind::Offline => "offline",
            Kind::Relay => "relay",
            Kind::Signin => "signin",
            Kind::NotMember => "not_member",
            Kind::TooYoung => "too_young",
            Kind::RateLimited => "rate_limited",
            Kind::TooBig => "too_big",
            Kind::Expired => "expired",
            Kind::NotFound => "not_found",
            Kind::BadRequest => "bad_request",
        }
    }
}

/// The `detail.kind` of an error from here, if it has one.
pub fn kind_of(e: &Error) -> Option<&str> {
    e.detail.as_ref()?.get("kind")?.as_str()
}

fn err(code: ErrorCode, kind: Kind, message: &str) -> Error {
    Error::new(code, message).with_detail(json!({ "kind": kind.as_str() }))
}

pub fn offline() -> Error {
    err(ErrorCode::Network, Kind::Offline, "You're offline.")
}

pub fn relay_down() -> Error {
    err(ErrorCode::Network, Kind::Relay, "Kiln support isn't answering right now.")
}

/// An answer that is not a success, as an error with the relay's reason (`{"error": kind, ...}`).
pub fn refusal(status: u16, body: &[u8]) -> Error {
    let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
    // the relay may spell its reasons with dashes (`not-member`)
    let reason = v.get("error").and_then(Value::as_str).unwrap_or("").replace('-', "_");
    let reason = reason.as_str();
    let num = |k: &str| v.get(k).and_then(Value::as_u64);
    match (status, reason) {
        (401, _) => err(ErrorCode::AuthFailed, Kind::Signin, "Sign in with Discord again."),
        (403, "not_member") => err(ErrorCode::Refused, Kind::NotMember, "You're not in the Kiln Discord yet."),
        (403, "too_young" | "too_new") => err(ErrorCode::Refused, Kind::TooYoung, "This Discord account is too new to send reports."),
        // the verifier, secret or one-time code is not this sign-in's: the UI words it as a relay that answered badly,
        // the sign-in tells it apart (a code a local page made up must not end the attempt)
        (403, "refused") => Error::new(ErrorCode::Refused, "Kiln support isn't answering right now.").with_detail(json!({ "kind": Kind::Relay.as_str(), "refused": true })),
        (429, "slow_down") => Error::new(ErrorCode::Busy, "Asked too soon.").with_detail(json!({ "kind": Kind::Relay.as_str(), "slow": true })),
        (404, _) => err(ErrorCode::NotFound, Kind::NotFound, "Kiln support doesn't know this any more."),
        (410, _) => err(ErrorCode::Timeout, Kind::Expired, "This has expired."),
        (413, _) => Error::new(ErrorCode::Refused, "The report is too big to send.")
            .with_detail(json!({ "kind": Kind::TooBig.as_str(), "size": num("size"), "limit": num("limit").unwrap_or(ZIP_MAX) })),
        (429, _) => {
            let last = v.get("last").or_else(|| v.get("lastPost")).and_then(Value::as_str).filter(|u| super::is_discord_url(u));
            // which limit (docs/relay-api.md: `hour` and `day` per account, `network` per network, a rolling 24 hours for
            // the last two) and until when (`retryAt`); the UI words each limit apart
            let limit = v.get("limit").and_then(Value::as_str).filter(|l| matches!(*l, "hour" | "day" | "network"));
            let sent = num("sent").or(match limit { Some("hour") => Some(3), Some("day") => Some(10), Some("network") => Some(20), _ => None });
            let wait = num("waitMin").or_else(|| {
                let at = crate::util::parse_rfc3339(v.get("retryAt")?.as_str()?)?;
                Some(((at - crate::util::now_unix()).max(60) as u64).div_ceil(60))
            });
            // the words fit every sending limit (reports, test results, Volunteer): the UI says more for a report
            Error::new(ErrorCode::Busy, "Kiln support asks you to wait before sending again.")
                .with_detail(json!({ "kind": Kind::RateLimited.as_str(), "limit": limit, "sent": sent, "waitMin": wait, "last": last }))
        }
        (400, _) => err(ErrorCode::InvalidArgument, Kind::BadRequest, "Kiln support could not read the request."),
        // a blocked account gets the same words as a relay that is down (PLAN 7.3)
        _ => relay_down(),
    }
}

/// `[A-Za-z0-9_-]{1,64}`: what may go into a path.
pub fn check_id(id: &str) -> Result<&str> {
    if !id.is_empty() && id.len() <= 64 && id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_') {
        Ok(id)
    } else {
        Err(Error::invalid("That id can't be sent to Kiln support."))
    }
}

/// The session key as the relay gave it: printable ASCII without spaces, 16 to 512 characters.
pub fn check_session(key: &str) -> Result<&str> {
    if (16..=512).contains(&key.len()) && key.bytes().all(|b| (0x21..=0x7e).contains(&b)) {
        Ok(key)
    } else {
        Err(Error::new(ErrorCode::Parse, "Kiln support sent a sign-in the launcher cannot keep."))
    }
}

// ---------------------------------------------------------------- shapes

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SigninMode {
    /// Desktop: the system browser, back through a one-time listener on 127.0.0.1.
    Browser,
    /// Deck: a QR code and a short code for a phone.
    Qr,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Device {
    /// A desktop or laptop (the relay's word: `desktop`).
    #[serde(rename = "desktop")]
    Pc,
    Deck,
}

/// `POST /signin`.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SigninRequest {
    pub mode: SigninMode,
    /// Also join the Kiln Discord (asks Discord for `guilds.join`).
    pub join: bool,
    /// `base64url(sha256(secret))`; the secret proves the attempt is this launcher's at the exchange and at each poll.
    pub challenge: String,
    /// Browser mode: the loopback port the relay sends the browser back to.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub port: Option<u16>,
    pub device: Device,
    /// The launcher's version (the phone page shows it).
    pub version: String,
}

/// The answer to `POST /signin`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SigninStarted {
    pub id: String,
    /// The short code both screens show (QR pairing); empty in browser mode.
    #[serde(default)]
    pub code: String,
    /// What the browser or the phone opens (https on frostdev.io).
    pub url: String,
    pub expires_at: String,
}

impl SigninStarted {
    fn checked(self, base: &RelayBase) -> Result<SigninStarted> {
        let site_ok = crate::net::policy::parse_url(&self.url).is_ok_and(|u| u.host == base.host || u.host == super::settings::RELAY_SITE);
        let code_ok = self.code.len() <= 12 && self.code.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'-');
        if check_id(&self.id).is_err() || !site_ok || !code_ok || crate::util::parse_rfc3339(&self.expires_at).is_none() {
            return Err(relay_down());
        }
        Ok(self)
    }
}

/// A session the relay handed out.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct Grant {
    /// The session key (the relay's field may be `token`).
    #[serde(alias = "token")]
    pub session: String,
    pub account: Account,
}

impl Grant {
    fn checked(self) -> Result<Grant> {
        check_session(&self.session)?;
        self.account.validate()?;
        Ok(self)
    }
}

/// `POST /signin/poll`'s answer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PollState {
    Waiting,
    Approved(Grant),
    Denied,
    Expired,
    Canceled,
}

// ---------------------------------------------------------------- the transport for a relay on this machine

/// Plain HTTP to one address on this machine: the tests' fake relay, and (debug builds only, see the launcher) a relay
/// a developer runs locally. It sends what [`net::Client`] checked, to `addr` instead of the URL's host.
pub struct LoopbackTransport {
    addr: std::net::SocketAddr,
}

impl LoopbackTransport {
    /// Refuses anything but a loopback address.
    pub fn new(addr: std::net::SocketAddr) -> Result<LoopbackTransport> {
        if !addr.ip().is_loopback() {
            return Err(Error::new(ErrorCode::Refused, "Only a relay on this machine can be reached without https."));
        }
        Ok(LoopbackTransport { addr })
    }
}

impl Transport for LoopbackTransport {
    fn exchange(&self, req: net::WireRequest) -> Result<net::Response> {
        use std::io::{Read, Write};
        let fail = || net::network_error(&req.url);
        let mut c = std::net::TcpStream::connect_timeout(&self.addr, std::time::Duration::from_secs(5)).map_err(|_| fail())?;
        c.set_read_timeout(Some(std::time::Duration::from_secs(10))).map_err(|_| fail())?;
        let mut head = format!("{} {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n", req.method.as_str(), req.url.path_and_query, req.url.host);
        for (k, v) in &req.headers {
            head.push_str(&format!("{k}: {v}\r\n"));
        }
        let len = req.body.len();
        if len > 0 || matches!(req.method, Method::Post | Method::Put | Method::Patch | Method::Delete) {
            head.push_str(&format!("Content-Length: {len}\r\n"));
        }
        head.push_str("\r\n");
        c.write_all(head.as_bytes()).map_err(|_| fail())?;
        match req.body {
            Body::Empty => {}
            Body::Bytes(b) => c.write_all(&b).map_err(|_| fail())?,
            Body::File { file, len } => {
                std::io::copy(&mut net::Exactly::new(file, len), &mut c).map_err(|_| fail())?;
            }
            Body::Reader { reader, len } => {
                std::io::copy(&mut net::Exactly::new(reader, len), &mut c).map_err(|_| fail())?;
            }
        }
        let mut raw = Vec::new();
        c.take(req.max_response + 64 * 1024).read_to_end(&mut raw).map_err(|_| fail())?;
        let end = raw.windows(4).position(|w| w == b"\r\n\r\n").ok_or_else(fail)?;
        let text = String::from_utf8_lossy(&raw[..end]).into_owned();
        let mut lines = text.split("\r\n");
        let status: u16 = lines.next().and_then(|l| l.split(' ').nth(1)).and_then(|s| s.parse().ok()).ok_or_else(fail)?;
        let headers: Vec<(String, String)> = lines.filter_map(|l| l.split_once(':')).map(|(k, v)| (k.trim().to_string(), v.trim().to_string())).collect();
        let mut body = raw[end + 4..].to_vec();
        if let Some(n) = headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("content-length")).and_then(|(_, v)| v.parse::<usize>().ok()) {
            body.truncate(n);
        }
        if body.len() as u64 > req.max_response {
            return Err(net::too_large(&req.url));
        }
        Ok(net::Response { status, headers, body })
    }
}

// ---------------------------------------------------------------- the client

/// A request body: JSON, or a multipart form (a report with its zip).
pub enum Payload {
    None,
    Json(Value),
    /// (name, content type, file name, bytes)
    Form(Vec<(String, String, Option<String>, Vec<u8>)>),
}

/// What goes with a report's draft.
pub enum Attachment<'z> {
    /// (file name, the zip): the team's copy, or attached to the public post.
    Zip(&'z str, &'z [u8]),
    /// report.json alone, for the post, when nobody gets the zip.
    ReportJson(&'z [u8]),
}

pub struct Relay<'a> {
    client: net::Client<'a>,
    base: RelayBase,
    session: Option<String>,
}

impl<'a> Relay<'a> {
    /// `version`: the launcher's (the User-Agent says `kiln-launcher/<version>` and nothing else).
    pub fn new(base: RelayBase, transport: &'a dyn Transport, version: &str) -> Relay<'a> {
        let policy = net::Policy::new("Kiln support").credential_host(&base.host);
        Relay { client: net::Client::new(policy, transport, &format!("kiln-launcher/{version}")).allow_delete(), base, session: None }
    }

    pub fn with_session(mut self, key: &str) -> Relay<'a> {
        self.session = Some(key.to_string());
        self
    }

    pub fn base(&self) -> &RelayBase {
        &self.base
    }

    /// One call: the answer's body on a 2xx, else the refusal as an error.
    pub fn call(&self, method: Method, path: &str, payload: Payload, auth: bool, max: u64) -> Result<Vec<u8>> {
        let mut req = Request::new(method, self.base.url(path)).max_response(max).header("Accept", "application/json");
        if auth {
            let key = self.session.as_deref().ok_or_else(|| err(ErrorCode::AuthFailed, Kind::Signin, "Sign in with Discord first."))?;
            req = req.header("Authorization", &format!("Bearer {key}"));
        }
        req = match payload {
            Payload::None => req,
            Payload::Json(v) => req.json(&v),
            Payload::Form(parts) => {
                let boundary = format!("kiln-{}", crate::hash::hex(&crate::util::random_bytes::<12>()?));
                let body = form_body(&boundary, &parts);
                req.header("Content-Type", &format!("multipart/form-data; boundary={boundary}")).body(Body::Bytes(body))
            }
        };
        let resp = self.client.send(req).map_err(|e| match e.code {
            ErrorCode::Network => offline(),
            ErrorCode::Timeout => relay_down(),
            // too large an answer, a redirect: not the relay we know
            _ if e.code == ErrorCode::Refused && !e.message.contains("Nothing was sent") => relay_down(),
            _ => e,
        })?;
        if (200..300).contains(&resp.status) {
            Ok(resp.body)
        } else {
            Err(refusal(resp.status, &resp.body))
        }
    }

    fn json<T: for<'de> Deserialize<'de>>(&self, method: Method, path: &str, payload: Payload, auth: bool, max: u64) -> Result<T> {
        let body = self.call(method, path, payload, auth, max)?;
        serde_json::from_slice(&body).map_err(|_| relay_down())
    }

    // ---- sign-in

    pub fn signin_start(&self, req: &SigninRequest) -> Result<SigninStarted> {
        let v = serde_json::to_value(req)?;
        self.json::<SigninStarted>(Method::Post, "/signin", Payload::Json(v), false, ANSWER_MAX)?.checked(&self.base)
    }

    /// Desktop: the one-time code from the loopback and the secret behind the challenge.
    pub fn signin_exchange(&self, id: &str, code: &str, verifier: &str) -> Result<Grant> {
        let v = json!({ "id": check_id(id)?, "code": code, "verifier": verifier });
        self.json::<Grant>(Method::Post, "/signin/exchange", Payload::Json(v), false, ANSWER_MAX)?.checked()
    }

    /// Deck (and the desktop's fallback when the loopback is blocked): whether the attempt was approved.
    pub fn signin_poll(&self, id: &str, secret: &str) -> Result<PollState> {
        #[derive(Deserialize)]
        struct Answer {
            state: String,
            #[serde(default, alias = "token")]
            session: Option<String>,
            #[serde(default)]
            account: Option<Account>,
        }
        let v = json!({ "id": check_id(id)?, "secret": secret });
        let a: Answer = match self.json(Method::Post, "/signin/poll", Payload::Json(v), false, ANSWER_MAX) {
            Err(e) if kind_of(&e) == Some("expired") || kind_of(&e) == Some("not_found") => return Ok(PollState::Expired),
            // asked again too soon: still waiting
            Err(e) if e.detail.as_ref().and_then(|d| d.get("slow")).is_some() => return Ok(PollState::Waiting),
            r => r?,
        };
        Ok(match a.state.as_str() {
            "waiting" => PollState::Waiting,
            "approved" => match (a.session, a.account) {
                (Some(session), Some(account)) => PollState::Approved(Grant { session, account }.checked()?),
                _ => return Err(relay_down()),
            },
            "denied" => PollState::Denied,
            "canceled" | "cancelled" => PollState::Canceled,
            "expired" => PollState::Expired,
            _ => return Err(relay_down()),
        })
    }

    // ---- the account

    pub fn me(&self) -> Result<Account> {
        #[derive(Deserialize)]
        struct Answer {
            account: Account,
        }
        let a: Answer = self.json(Method::Get, "/me", Payload::None, true, ANSWER_MAX)?;
        a.account.validate()?;
        Ok(a.account)
    }

    pub fn signout(&self) -> Result<()> {
        self.call(Method::Delete, "/session", Payload::None, true, ANSWER_MAX).map(drop)
    }

    /// "Sign out and forget me"; `posts`: also delete the player's support posts on Discord.
    pub fn forget(&self, posts: bool) -> Result<()> {
        self.call(Method::Delete, if posts { "/me?posts=1" } else { "/me?posts=0" }, Payload::None, true, ANSWER_MAX).map(drop)
    }

    // ---- reports and posts

    /// `draft`: [`super::post::wire`]; `with`: the zip (file name, bytes), or report.json alone when the player chose
    /// that nobody gets the zip ([`crate::report::public_json`]): the relay builds the post from either.
    pub fn report(&self, draft: Value, with: Attachment) -> Result<SupportPost> {
        if let Attachment::Zip(_, z) = &with
            && z.len() as u64 > ZIP_MAX
        {
            return Err(refusal(413, json!({ "error": "too_big", "size": z.len(), "limit": ZIP_MAX }).to_string().as_bytes()));
        }
        let mut parts = vec![("draft".to_string(), "application/json".to_string(), None, draft.to_string().into_bytes())];
        match with {
            Attachment::Zip(name, z) => parts.push(("zip".to_string(), "application/zip".to_string(), Some(name.to_string()), z.to_vec())),
            Attachment::ReportJson(j) => parts.push(("report".to_string(), "application/json".to_string(), Some("report.json".to_string()), j.to_vec())),
        }
        #[derive(Deserialize)]
        struct Answer {
            post: SupportPost,
        }
        let a: Answer = self.json(Method::Post, "/reports", Payload::Form(parts), true, ANSWER_MAX)?;
        a.post.validate()?;
        Ok(a.post)
    }

    pub fn posts(&self) -> Result<Vec<SupportPost>> {
        #[derive(Deserialize)]
        struct Answer {
            posts: Vec<SupportPost>,
        }
        let a: Answer = self.json(Method::Get, "/posts", Payload::None, true, ANSWER_MAX)?;
        // a post that can't be shown is left out, not the whole list
        Ok(a.posts.into_iter().filter(|p| p.validate().is_ok()).take(200).collect())
    }

    pub fn post_seen(&self, id: &str) -> Result<()> {
        self.call(Method::Post, &format!("/posts/{}/seen", check_id(id)?), Payload::None, true, ANSWER_MAX).map(drop)
    }

    pub fn post_solved(&self, id: &str) -> Result<()> {
        self.call(Method::Post, &format!("/posts/{}/solved", check_id(id)?), Payload::None, true, ANSWER_MAX).map(drop)
    }

    pub fn post_delete(&self, id: &str) -> Result<()> {
        self.call(Method::Delete, &format!("/posts/{}", check_id(id)?), Payload::None, true, ANSWER_MAX).map(drop)
    }

    // ---- public

    /// News and events (no session is sent, even when there is one).
    pub fn feed(&self) -> Result<Feed> {
        Ok(self.json::<Feed>(Method::Get, "/feed", Payload::None, false, ANSWER_MAX)?.cleaned())
    }

    /// The known-problems list (no session is sent).
    pub fn known_issues(&self) -> Result<super::known::KnownList> {
        let body = self.call(Method::Get, "/known-issues", Payload::None, false, KNOWN_MAX)?;
        super::known::KnownList::parse(&body).map_err(|_| relay_down())
    }

    // ---- testers

    pub fn tester(&self) -> Result<super::testers::RemoteTester> {
        self.json(Method::Get, "/tester", Payload::None, true, ANSWER_MAX)
    }

    pub fn tester_join(&self, hardware: &super::hardware::Hardware, beta: bool) -> Result<()> {
        let v = json!({ "hardware": hardware.wire(), "beta": beta });
        self.call(Method::Post, "/tester", Payload::Json(v), true, ANSWER_MAX).map(drop)
    }

    // (`GET /tester` answers `{tester, hardware, results}`; a `plan` beside them is read when the relay serves one.)

    pub fn tester_leave(&self) -> Result<()> {
        self.call(Method::Delete, "/tester", Payload::None, true, ANSWER_MAX).map(drop)
    }

    pub fn tester_results(&self, results: Value, zip: Option<(&str, &[u8])>) -> Result<super::testers::ResultMark> {
        let mut parts = vec![("results".to_string(), "application/json".to_string(), None, results.to_string().into_bytes())];
        if let Some((name, z)) = zip {
            parts.push(("zip".to_string(), "application/zip".to_string(), Some(name.to_string()), z.to_vec()));
        }
        #[derive(Deserialize)]
        struct Answer {
            result: super::testers::ResultMark,
        }
        Ok(self.json::<Answer>(Method::Post, "/tester/results", Payload::Form(parts), true, ANSWER_MAX)?.result)
    }
}

fn form_body(boundary: &str, parts: &[(String, String, Option<String>, Vec<u8>)]) -> Vec<u8> {
    let mut out = Vec::new();
    for (name, ctype, file, bytes) in parts {
        let file = file.as_ref().map(|f| format!("; filename=\"{}\"", f.replace(['"', '\r', '\n'], ""))).unwrap_or_default();
        out.extend_from_slice(format!("--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"{file}\r\nContent-Type: {ctype}\r\n\r\n").as_bytes());
        out.extend_from_slice(bytes);
        out.extend_from_slice(b"\r\n");
    }
    out.extend_from_slice(format!("--{boundary}--\r\n").as_bytes());
    out
}

#[cfg(test)]
mod tests {
    use super::super::testing::{multipart, ok, status, FakeRelay};
    use super::*;

    const KEY: &str = "kiln_s_0123456789abcdefghijklmnop";

    fn base() -> RelayBase {
        super::super::settings::relay_base(super::super::settings::DEFAULT_RELAY).unwrap()
    }

    fn account() -> Value {
        json!({ "id": "731204485922648124", "name": "emberfox", "member": true, "tester": false })
    }

    #[test]
    fn me_sends_the_session_key_and_the_version_only() {
        let fake = FakeRelay::start(|_| ok(json!({ "account": account() })));
        let t = fake.transport();
        let a = Relay::new(base(), &t, "0.2.0-dev.29.18").with_session(KEY).me().unwrap();
        assert_eq!(a.name, "emberfox");
        let seen = fake.seen();
        assert_eq!(seen.len(), 1);
        assert_eq!((seen[0].method.as_str(), seen[0].target.as_str()), ("GET", "/kiln/relay/me"));
        assert_eq!(seen[0].header("authorization"), Some(format!("Bearer {KEY}").as_str()));
        assert_eq!(seen[0].header("user-agent"), Some("kiln-launcher/0.2.0-dev.29.18"));
        assert_eq!(seen[0].header("host"), Some("frostdev.io"));
        assert!(seen[0].header("cookie").is_none() && seen[0].header("referer").is_none());
    }

    #[test]
    fn the_public_feed_never_carries_the_session() {
        let fake = FakeRelay::start(|_| ok(json!({ "releases": [{ "title": "Kiln 0.2.0-dev.29.18", "date": "2026-11-17T10:00:00Z", "line": "Notes.", "url": "https://frostdev.io/kiln/download" }], "events": [] })));
        let t = fake.transport();
        let f = Relay::new(base(), &t, "1").with_session(KEY).feed().unwrap();
        assert_eq!(f.releases.len(), 1);
        assert!(fake.seen()[0].header("authorization").is_none());
    }

    #[test]
    fn refusals_become_the_kinds_the_ui_words() {
        let cases: Vec<(u16, Value, &str)> = vec![
            (401, json!({}), "signin"),
            (403, json!({ "error": "not_member" }), "not_member"),
            (403, json!({ "error": "too_young" }), "too_young"),
            (403, json!({ "error": "blocked" }), "relay"),
            (404, json!({}), "not_found"),
            (413, json!({ "error": "too_big", "size": 21_400_000, "limit": 8_388_608 }), "too_big"),
            (429, json!({ "error": "rate_limited", "sent": 3, "waitMin": 38, "last": "https://discord.com/channels/1/2" }), "rate_limited"),
            (500, json!({}), "relay"),
            (502, Value::Null, "relay"),
        ];
        for (code, body, kind) in cases {
            let b = body.clone();
            let fake = FakeRelay::start(move |_| status(code, b.clone()));
            let t = fake.transport();
            let e = Relay::new(base(), &t, "1").with_session(KEY).posts().unwrap_err();
            assert_eq!(kind_of(&e), Some(kind), "{code} {body}");
            if kind == "rate_limited" {
                let d = e.detail.clone().unwrap();
                assert_eq!((d["sent"].as_u64(), d["waitMin"].as_u64(), d["last"].as_str()), (Some(3), Some(38), Some("https://discord.com/channels/1/2")));
            }
            if kind == "too_big" {
                assert_eq!(e.detail.unwrap()["limit"], 8_388_608);
            }
        }
    }

    #[test]
    fn the_relays_dashed_reasons_and_its_retry_time_read_the_same() {
        assert_eq!(kind_of(&refusal(403, br#"{"error":"not-member","invite":"https://discord.gg/7pjr8SMTfU"}"#)), Some("not_member"));
        assert_eq!(kind_of(&refusal(403, br#"{"error":"too-new","retryAt":"2026-11-20T00:00:00.000Z"}"#)), Some("too_young"));
        assert_eq!(kind_of(&refusal(401, br#"{"error":"signed-out"}"#)), Some("signin"));
        assert_eq!(kind_of(&refusal(503, br#"{"error":"unavailable"}"#)), Some("relay"));
        let at = crate::util::rfc3339(crate::util::now_unix() + 38 * 60 - 10);
        let e = refusal(429, format!(r#"{{"error":"rate-limited","limit":"hour","retryAt":"{at}","lastPost":"https://discord.com/channels/1/2"}}"#).as_bytes());
        let d = e.detail.unwrap();
        assert_eq!((d["kind"].as_str(), d["sent"].as_u64(), d["waitMin"].as_u64(), d["last"].as_str()), (Some("rate_limited"), Some(3), Some(38), Some("https://discord.com/channels/1/2")));
        assert_eq!(d["limit"], "hour");
        // the day's and the network's limits say which they are (the UI words them apart: not "in the last hour")
        let at = crate::util::rfc3339(crate::util::now_unix() + 5 * 3600);
        for (limit, sent) in [("day", 10), ("network", 20)] {
            let d = refusal(429, format!(r#"{{"error":"rate-limited","limit":"{limit}","retryAt":"{at}","lastPost":null}}"#).as_bytes()).detail.unwrap();
            assert_eq!((d["limit"].as_str(), d["sent"].as_u64(), d["waitMin"].as_u64(), d["last"].is_null()), (Some(limit), Some(sent), Some(300), true));
        }
        // a limit the launcher doesn't know is left out, not guessed
        assert!(refusal(429, br#"{"error":"rate-limited","limit":"week"}"#).detail.unwrap()["limit"].is_null());
        // the session's field may be called token
        let g: Grant = serde_json::from_value(json!({ "token": KEY, "account": account() })).unwrap();
        assert_eq!(g.session, KEY);
    }

    #[test]
    fn a_rate_limit_link_that_is_not_discords_is_dropped() {
        let e = refusal(429, br#"{"error":"rate_limited","sent":3,"waitMin":5,"last":"https://evil.example/x"}"#);
        assert!(e.detail.unwrap()["last"].is_null());
    }

    #[test]
    fn no_answer_is_offline() {
        // a port nobody listens on
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = l.local_addr().unwrap();
        drop(l);
        let t = LoopbackTransport::new(addr).unwrap();
        let e = Relay::new(base(), &t, "1").feed().unwrap_err();
        assert_eq!(kind_of(&e), Some("offline"));
    }

    #[test]
    fn nonsense_from_the_relay_is_relay_down() {
        let fake = FakeRelay::start(|_| super::super::testing::Answer { status: 200, body: "<html>".into() });
        let t = fake.transport();
        assert_eq!(kind_of(&Relay::new(base(), &t, "1").with_session(KEY).posts().unwrap_err()), Some("relay"));
    }

    #[test]
    fn calls_that_need_a_session_send_nothing_without_one() {
        let fake = FakeRelay::start(|_| ok(json!({ "posts": [] })));
        let t = fake.transport();
        let e = Relay::new(base(), &t, "1").posts().unwrap_err();
        assert_eq!(kind_of(&e), Some("signin"));
        assert!(fake.seen().is_empty());
    }

    #[test]
    fn an_id_from_elsewhere_never_shapes_the_path() {
        let fake = FakeRelay::start(|_| status(204, Value::Null));
        let t = fake.transport();
        let r = Relay::new(base(), &t, "1").with_session(KEY);
        for bad in ["../me", "a/b", "x?y=1", "", "p%2e%2e"] {
            assert!(r.post_delete(bad).is_err(), "{bad}");
        }
        assert!(fake.seen().is_empty());
        r.post_delete("p3").unwrap();
        r.post_seen("p3").unwrap();
        r.post_solved("p3").unwrap();
        let seen: Vec<(String, String)> = fake.seen().iter().map(|s| (s.method.clone(), s.path().to_string())).collect();
        assert_eq!(seen, [("DELETE".into(), "/posts/p3".into()), ("POST".into(), "/posts/p3/seen".into()), ("POST".into(), "/posts/p3/solved".into())]);
    }

    #[test]
    fn posts_that_cannot_be_shown_are_left_out() {
        let fake = FakeRelay::start(|_| ok(json!({ "posts": [
            { "id": "p1", "title": "Mods don't load", "url": "https://discord.com/channels/1568454645683195976/1573130884210000003", "created": "2026-11-18T10:00:00Z", "status": "replied", "replies": 3, "unread": 1,
              "lastReply": { "name": "Rowan", "team": true, "at": "2026-11-18T11:00:00Z" }, "tags": ["Mods", "Linux"], "zip": "team" },
            { "id": "p2", "title": "Evil", "url": "https://evil.example/x", "created": "2026-11-18T10:00:00Z", "status": "waiting" },
        ] })));
        let t = fake.transport();
        let posts = Relay::new(base(), &t, "1").with_session(KEY).posts().unwrap();
        assert_eq!(posts.len(), 1);
        assert_eq!(posts[0].unread, 1);
        assert_eq!(posts[0].last_reply.as_ref().unwrap().name, "Rowan");
    }

    #[test]
    fn a_report_is_a_form_with_the_draft_and_the_zip() {
        let fake = FakeRelay::start(|_| status(201, json!({ "post": { "id": "p9", "title": "The game crashes", "url": "https://discord.com/channels/1/9", "created": "2026-11-18T12:00:00Z", "status": "waiting" } })));
        let t = fake.transport();
        let post = Relay::new(base(), &t, "1").with_session(KEY).report(json!({ "title": "The game crashes" }), Attachment::Zip("kiln-report-2026-11-18.zip", b"PK\x03\x04zip")).unwrap();
        assert_eq!(post.id, "p9");
        let seen = &fake.seen()[0];
        assert_eq!(seen.path(), "/reports");
        let parts = multipart(seen);
        assert_eq!(parts.iter().map(|p| p.0.as_str()).collect::<Vec<_>>(), ["draft", "zip"]);
        assert_eq!(parts[0].1, "application/json");
        assert_eq!(serde_json::from_slice::<Value>(&parts[0].2).unwrap()["title"], "The game crashes");
        assert_eq!(parts[1].2, b"PK\x03\x04zip");
    }

    #[test]
    fn without_the_zip_report_json_goes_alone_as_the_report_part() {
        let fake = FakeRelay::start(|_| status(201, json!({ "post": { "id": "1573990884210000077", "title": "x y z", "url": "https://discord.com/channels/1/9", "created": "2026-11-18T12:00:00.000Z", "status": "waiting", "replies": 0, "unread": 0, "lastReply": null, "zip": "none" }, "tags": ["Mods", "Linux"], "reportId": "KR-7F3Q", "known": null })));
        let t = fake.transport();
        Relay::new(base(), &t, "1").with_session(KEY).report(json!({ "title": "x y z", "kind": "mod", "zip": "none" }), Attachment::ReportJson(br#"{"format":1}"#)).unwrap();
        let parts = multipart(&fake.seen()[0]);
        assert_eq!(parts.iter().map(|p| p.0.as_str()).collect::<Vec<_>>(), ["draft", "report"]);
        let head = String::from_utf8_lossy(&fake.seen()[0].body).into_owned();
        assert!(head.contains("name=\"report\"; filename=\"report.json\""), "the relay takes it as a file");
    }

    #[test]
    fn a_zip_over_the_limit_is_refused_before_sending() {
        let fake = FakeRelay::start(|_| status(201, json!({})));
        let t = fake.transport();
        let big = vec![0u8; ZIP_MAX as usize + 1];
        let e = Relay::new(base(), &t, "1").with_session(KEY).report(json!({}), Attachment::Zip("r.zip", &big)).unwrap_err();
        assert_eq!(kind_of(&e), Some("too_big"));
        assert!(fake.seen().is_empty());
    }

    #[test]
    fn signout_and_forget_are_deletes() {
        let fake = FakeRelay::start(|_| status(204, Value::Null));
        let t = fake.transport();
        let r = Relay::new(base(), &t, "1").with_session(KEY);
        r.signout().unwrap();
        r.forget(true).unwrap();
        r.forget(false).unwrap();
        let seen: Vec<String> = fake.seen().iter().map(|s| format!("{} {}", s.method, s.target)).collect();
        assert_eq!(seen, ["DELETE /kiln/relay/session", "DELETE /kiln/relay/me?posts=1", "DELETE /kiln/relay/me?posts=0"]);
    }

    #[test]
    fn a_session_key_that_is_not_one_is_not_kept() {
        let fake = FakeRelay::start(|_| ok(json!({ "state": "approved", "session": "short", "account": account() })));
        let t = fake.transport();
        assert!(Relay::new(base(), &t, "1").signin_poll("s1", "secret").is_err());
    }

    #[test]
    fn a_signin_link_off_frostdev_io_is_refused() {
        let fake = FakeRelay::start(|_| ok(json!({ "id": "s1", "code": "HV3R-9TCW", "url": "https://evil.example/link", "expiresAt": "2026-11-18T12:10:00Z" })));
        let t = fake.transport();
        let req = SigninRequest { mode: SigninMode::Qr, join: false, challenge: "c".into(), port: None, device: Device::Deck, version: "1".into() };
        assert_eq!(kind_of(&Relay::new(base(), &t, "1").signin_start(&req).unwrap_err()), Some("relay"));
    }

    #[test]
    fn the_loopback_transport_only_reaches_this_machine() {
        assert!(LoopbackTransport::new("10.0.0.1:80".parse().unwrap()).is_err());
        assert!(LoopbackTransport::new("127.0.0.1:80".parse().unwrap()).is_ok());
    }
}
