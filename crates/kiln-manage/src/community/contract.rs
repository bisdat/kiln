//! The relay's contract, example by example: `relay-contract.json` is the kilnbot repo's docs/relay-contract.json
//! (master 3eb30a7, the relay live on frostdev.io since 18 November 2026), the examples its own test checks against the
//! real relay. For each one the launcher's client makes the call against a strict fake relay ([`FakeRelay::strict`]:
//! the real relay's rules for every request, [`super::testing::relay_rules`]), which answers with the example's
//! answer. The test checks that the request has the example's method, path and fields, that the relay would take it,
//! and that the launcher reads the answer as the UI needs it. A new example in the file fails here until the launcher
//! is checked against it. Nothing leaves the machine.

use super::hardware::{Capability, Hardware};
use super::relay::{kind_of, Attachment, Device, PollState, Relay, SigninMode, SigninRequest};
use super::settings::{relay_base, DEFAULT_RELAY};
use super::testing::{multipart, Answer, FakeRelay, Seen};
use serde_json::{json, Value};

const CONTRACT: &str = include_str!("relay-contract.json");
const ID: &str = "Zm9vYmFyYmF6cXV4cXV1eGNvcmdlZ3JhdWx0Z2FycGx";
const TOKEN: &str = "a2lsbi1zZXNzaW9uLWtleS1vZi1mb3J0eS10aHJlZS1";
const SNOWFLAKE: &str = "1573130884210000003";
const POST_URL: &str = "https://discord.com/channels/1568454645683195976/1573130884210000003";

/// The example's placeholders as concrete values (the file's own `placeholders`).
fn fill(v: &Value) -> Value {
    match v {
        Value::String(s) => match s.as_str() {
            "<number>" => json!(4_194_304),
            "<any>" => Value::Null,
            "<url>" => json!(POST_URL),
            _ => json!(s
                .replace("<id>", ID)
                .replace("<token>", TOKEN)
                .replace("<snowflake>", SNOWFLAKE)
                .replace("<time>", "2026-11-18T12:00:00.000Z")
                .replace("<code8>", "KQ7M4P2X")
                .replace("<code>", "KQ7M-4P2X")
                .replace("<report>", "KR-7F3Q")
                .replace("<text>", "Words for people.")),
        },
        Value::Array(a) => Value::Array(a.iter().map(fill).collect()),
        Value::Object(o) => Value::Object(o.iter().map(|(k, x)| (k.clone(), fill(x))).collect()),
        x => x.clone(),
    }
}

fn keys(v: &Value) -> Vec<String> {
    let mut k: Vec<String> = v.as_object().map(|o| o.keys().cloned().collect()).unwrap_or_default();
    k.sort();
    k
}

fn hardware() -> Hardware {
    Hardware { gpu: "AMD Radeon RX 7800 XT".into(), driver: "Mesa 26.2.1 (RADV)".into(), vram: "16 GB".into(), os: "Arch Linux".into(),
        runner: "GE-Proton10-3".into(), caps: vec![Capability::Fsr4], covers: vec!["FSR 4".into()] }
}

/// What the launcher's call gave: its answer as JSON (for the checks below) or the error's `detail`.
type Got = Result<Value, Value>;

fn detail(e: crate::Error) -> Value {
    e.detail.unwrap_or(Value::Null)
}

/// The launcher's call for an example, as the launcher makes it.
fn call(name: &str, r: &Relay) -> Got {
    let start = |mode, port, device| SigninRequest { mode, join: true, challenge: ID.into(), port, device, version: "0.2.0-dev.30".into() };
    match name {
        "start a desktop sign-in" => r.signin_start(&start(SigninMode::Browser, Some(51234), Device::Pc)).map(|s| json!({ "id": s.id, "url": s.url })).map_err(detail),
        "start a Deck sign-in (QR pairing)" | "too many sign-ins from one network" => {
            r.signin_start(&start(SigninMode::Qr, None, Device::Deck)).map(|s| json!({ "id": s.id, "code": s.code, "url": s.url })).map_err(detail)
        }
        "finish a desktop sign-in with the loopback's one-time code" | "exchange with the wrong verifier" => {
            r.signin_exchange(ID, ID, ID).map(|g| json!({ "session": g.session, "name": g.account.name })).map_err(detail)
        }
        n if n.starts_with("poll ") => r
            .signin_poll(ID, ID)
            .map(|p| match p {
                PollState::Waiting => json!("waiting"),
                PollState::Approved(g) => json!({ "approved": g.session }),
                PollState::Denied => json!("denied"),
                PollState::Canceled => json!("canceled"),
                PollState::Expired => json!("expired"),
            })
            .map_err(detail),
        "the account" | "the account, signed out" => r.me().map(|a| json!({ "name": a.name, "member": a.member })).map_err(detail),
        n if n.starts_with("send a report") => {
            let draft = json!({ "title": "Mods don't load since the game update", "kind": "noload", "text": "It worked yesterday.", "zip": "team" });
            r.report(draft, Attachment::Zip("kiln-report-2026-11-18.zip", b"PK\x05\x06")).map(|p| json!({ "id": p.id, "url": p.url })).map_err(detail)
        }
        "your posts" => r.posts().map(|p| json!(p.len())).map_err(detail),
        "opened a post" | "not your post" => r.post_seen(SNOWFLAKE).map(|_| Value::Null).map_err(detail),
        "It's fixed" => r.post_solved(SNOWFLAKE).map(|_| Value::Null).map_err(detail),
        "Delete my post" => r.post_delete(SNOWFLAKE).map(|_| Value::Null).map_err(detail),
        "news and events (public)" => r.feed().map(|f| json!([f.releases.len(), f.events.len()])).map_err(detail),
        "known problems (public)" => r.known_issues().map(|l| json!(l.issues.len())).map_err(detail),
        "tester state" | "tester state with a read mark" => {
            r.tester().map(|t| json!({ "tester": t.tester, "read": t.results.first().and_then(|x| x.read.as_ref()).map(|m| m.by.clone()) })).map_err(detail)
        }
        "volunteer as a tester" => r.tester_join(&hardware(), true).map(|_| Value::Null).map_err(detail),
        "send test results" | "test results for a build without a #testing post" => {
            let results = json!({ "build": "0.2.0-dev.30", "steps": [{ "id": "fsr4", "title": "Turn on FSR 4", "result": "failed", "note": "Water smears" }] });
            r.tester_results(results, None).map(|m| json!(m.build)).map_err(detail)
        }
        "stop testing" => r.tester_leave().map(|_| Value::Null).map_err(detail),
        "sign out" => r.signout().map(|_| Value::Null).map_err(detail),
        "sign out and forget me (and delete my posts)" => r.forget(true).map(|_| Value::Null).map_err(detail),
        other => panic!("the contract has an example the launcher is not checked against: {other}"),
    }
}

/// What the UI needs from each answer.
fn expect(name: &str, got: &Got) {
    let kind = |g: &Got| g.as_ref().err().and_then(|d| d["kind"].as_str().map(str::to_string));
    match name {
        "start a desktop sign-in" => assert_eq!(got.as_ref().unwrap()["url"], format!("https://frostdev.io/kiln/signin/{ID}")),
        "start a Deck sign-in (QR pairing)" => assert_eq!(got.as_ref().unwrap(), &json!({ "id": ID, "code": "KQ7M-4P2X", "url": "https://frostdev.io/link/KQ7M4P2X" })),
        "too many sign-ins from one network" => {
            assert_eq!(kind(got).as_deref(), Some("rate_limited"));
            assert_eq!(got.as_ref().unwrap_err()["limit"], "network");
        }
        "finish a desktop sign-in with the loopback's one-time code" => assert_eq!(got.as_ref().unwrap(), &json!({ "session": TOKEN, "name": "Emberfox" })),
        "exchange with the wrong verifier" => assert_eq!(got.as_ref().unwrap_err()["refused"], true, "a refused code is told apart (it must not end a sign-in)"),
        "poll while waiting" | "poll too often" => assert_eq!(got.as_ref().unwrap(), "waiting"),
        "poll after Approve" => assert_eq!(got.as_ref().unwrap(), &json!({ "approved": TOKEN })),
        "poll after Decline on the phone" => assert_eq!(got.as_ref().unwrap(), "denied"),
        "poll after Cancel on Discord's page" => assert_eq!(got.as_ref().unwrap(), "canceled"),
        "poll an expired or unknown sign-in" => assert_eq!(got.as_ref().unwrap(), "expired"),
        "the account" => assert_eq!(got.as_ref().unwrap(), &json!({ "name": "Emberfox", "member": true })),
        "the account, signed out" => assert_eq!(kind(got).as_deref(), Some("signin")),
        "send a report (zip to the team)" => assert_eq!(got.as_ref().unwrap()["id"], SNOWFLAKE),
        "send a report, not in the server" => assert_eq!(kind(got).as_deref(), Some("not_member")),
        "send a report, account under 3 days old" => assert_eq!(kind(got).as_deref(), Some("too_young")),
        "send a report, not one of our zips" | "send a report, Discord not answering" => assert_eq!(kind(got).as_deref(), Some("relay")),
        "send a report, Discord refused it" => {
            let d = got.as_ref().unwrap_err();
            assert_eq!((d["kind"].as_str(), d["status"].as_u64(), d["code"].as_u64(), d["reason"].as_str()), (Some("discord"), Some(403), Some(50013), Some("Missing Permissions")));
        }
        "send a report, Discord rate-limits the relay" => {
            let d = got.as_ref().unwrap_err();
            assert_eq!((d["kind"].as_str(), d["limited"].as_bool(), d["waitMin"].as_u64()), (Some("discord"), Some(true), Some(1)));
        }
        "send a report, zip too big" => {
            let d = got.as_ref().unwrap_err();
            assert_eq!((d["kind"].as_str(), d["size"].as_u64(), d["limit"].as_u64()), (Some("too_big"), Some(4_194_304), Some(8_388_608)));
        }
        "send a report, too many this hour" => {
            let d = got.as_ref().unwrap_err();
            assert_eq!((d["kind"].as_str(), d["limit"].as_str(), d["sent"].as_u64(), d["last"].as_str()), (Some("rate_limited"), Some("hour"), Some(3), Some(POST_URL)));
        }
        "your posts" => assert_eq!(got.as_ref().unwrap(), 1, "the post is shown"),
        "opened a post" | "It's fixed" | "Delete my post" | "volunteer as a tester" | "stop testing" | "sign out" | "sign out and forget me (and delete my posts)" => {
            assert!(got.is_ok(), "{got:?}")
        }
        "not your post" => assert_eq!(kind(got).as_deref(), Some("not_found")),
        "news and events (public)" => assert_eq!(got.as_ref().unwrap(), &json!([1, 1])),
        "known problems (public)" => assert_eq!(got.as_ref().unwrap(), 1),
        "tester state" => assert_eq!(got.as_ref().unwrap(), &json!({ "tester": false, "read": null })),
        "tester state with a read mark" => assert_eq!(got.as_ref().unwrap(), &json!({ "tester": true, "read": "Rowan" })),
        "send test results" => assert_eq!(got.as_ref().unwrap(), "0.2.0-dev.30"),
        "test results for a build without a #testing post" => assert!(got.is_err()),
        other => panic!("no expectation for {other}"),
    }
}

/// The request the launcher sent, against the example's.
fn compare(name: &str, ex: &Value, seen: &Seen) {
    let path = fill(&ex["path"]);
    assert_eq!((seen.method.as_str(), seen.target.as_str()), (ex["method"].as_str().unwrap(), format!("/kiln/relay{}", path.as_str().unwrap()).as_str()), "{name}");
    // the session key goes exactly where the contract says ("the account, signed out" is the relay's answer to a key
    // it no longer knows: the launcher never asks for the account without one)
    let auth = ex["auth"].as_bool().unwrap_or(false) || name == "the account, signed out";
    assert_eq!(seen.header("authorization").is_some(), auth, "{name}: the session key goes exactly where the contract says");
    match &ex["request"] {
        // a JSON body: the example's fields, no others (the desktop's start sends exactly the example's; the Deck's adds
        // `device`, which the relay takes)
        Value::Object(_) => {
            let sent = seen.json();
            let mut want = keys(&ex["request"]);
            if name == "start a Deck sign-in (QR pairing)" {
                want.push("device".into());
                want.sort();
                assert_eq!(sent["device"], "deck");
            }
            if name != "too many sign-ins from one network" {
                assert_eq!(keys(&sent), want, "{name}: the fields");
            }
            if name == "volunteer as a tester" {
                assert_eq!(sent, ex["request"], "{name}: the hardware lines go as the volunteer screen showed them");
            }
        }
        // a form: the parts the example names
        Value::String(form) => {
            let parts: Vec<String> = multipart(seen).into_iter().map(|p| p.0).collect();
            for p in &parts {
                assert!(form.contains(&format!("{p} (")) || form.ends_with(&format!(", {p}")) || form.contains(&format!(": {p}")), "{name}: the part {p} is not in \"{form}\"");
            }
        }
        _ => assert!(seen.body.is_empty(), "{name}: no body"),
    }
}

#[test]
fn every_example_of_the_relays_contract_holds_for_the_launcher() {
    let contract: Value = serde_json::from_str(CONTRACT).unwrap();
    assert_eq!(contract["base"], DEFAULT_RELAY);
    let mut checked = 0;
    for ex in contract["endpoints"].as_array().unwrap() {
        let name = ex["name"].as_str().unwrap();
        if name == "health" {
            // the launcher never calls it
            continue;
        }
        let answer = fill(&ex["answer"]["body"]);
        let status = ex["answer"]["status"].as_u64().unwrap() as u16;
        let fake = FakeRelay::strict(move |_| Answer { status, body: answer.to_string() });
        let t = fake.transport();
        let relay = Relay::new(relay_base(DEFAULT_RELAY).unwrap(), &t, "0.2.0-dev.29.18").with_session(TOKEN);
        let got = call(name, &relay);
        assert_eq!(fake.broken(), Vec::<String>::new(), "{name}: the relay would refuse the launcher's request");
        let seen = fake.seen();
        assert_eq!(seen.len(), 1, "{name}: one request");
        compare(name, ex, &seen[0]);
        expect(name, &got);
        checked += 1;
    }
    assert_eq!(checked, 37, "every example but /health");
}

#[test]
fn the_strict_fake_relay_refuses_what_the_real_one_refuses() {
    // the rules themselves: an unknown field, a query, a body on a bodiless call (also a chunked one), a short key
    let fake = FakeRelay::strict(|_| Answer { status: 200, body: "{}".into() });
    let t = fake.transport();
    let relay = Relay::new(relay_base(DEFAULT_RELAY).unwrap(), &t, "0.2.0-dev.29.18").with_session(TOKEN);
    let send = |method, path: &str, body: Option<Value>| relay.call(method, path, body.map_or(super::relay::Payload::None, super::relay::Payload::Json), true, 1024);
    // an unknown field in a sign-in
    let e = relay.call(crate::net::Method::Post, "/signin", super::relay::Payload::Json(json!({ "mode": "qr", "challenge": ID, "colour": "red" })), false, 1024).unwrap_err();
    assert_eq!(kind_of(&e), Some("bad_request"));
    // a query where none is taken
    assert!(send(crate::net::Method::Get, "/posts?all=1", None).is_err());
    // a body on It's fixed
    assert!(send(crate::net::Method::Post, &format!("/posts/{SNOWFLAKE}/solved"), Some(json!({}))).is_err());
    // DELETE /me takes posts=1 or posts=0, nothing else
    assert!(send(crate::net::Method::Delete, "/me?posts=yes", None).is_err());
    assert!(send(crate::net::Method::Delete, "/me?posts=0", None).is_ok());
    // a session key that is not one
    let short = Relay::new(relay_base(DEFAULT_RELAY).unwrap(), &t, "1.0.0").with_session("kiln_s_0123456789abcdefghijklmnop");
    assert!(short.me().is_err());
    let broken = fake.broken();
    assert_eq!(broken.len(), 5, "{broken:#?}");
    assert!(broken.iter().any(|b| b.contains("unknown field colour")) && broken.iter().any(|b| b.contains("takes no body")) && broken.iter().any(|b| b.contains("43 base64url")), "{broken:#?}");
    // a chunked body (what ureq sent for an empty POST before the fix) breaks the bodiless rule too
    let chunked = Seen { method: "POST".into(), target: format!("/kiln/relay/posts/{SNOWFLAKE}/seen"),
        headers: vec![("Authorization".into(), format!("Bearer {TOKEN}")), ("User-Agent".into(), "kiln-launcher/1".into()), ("Transfer-Encoding".into(), "chunked".into())], body: vec![] };
    assert!(super::testing::relay_rules(&chunked).iter().any(|b| b.contains("takes no body")));
}
