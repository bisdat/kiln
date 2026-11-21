// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family, shared bodies.
// Shared network maintenance bodies: run by kiln-sfi's own tests (tests/runtime_network_maintenance_proof.rs)
// and by any other test package that includes them. Expected behaviour is
// ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), both pins (modules/transport is identical):
// native/steam_network.cpp:19-68,192-246,284-317,344-429,450-494; read_network_allowlist.rs:1-18;
// report_network_health.rs:35-85; lua/network.rs:259-303.
//! - Authorization: kiln-sfi's incremental server-log follower finds, after every chunk the log grows
//!   by, exactly the peers ShroudForge's whole-file scan finds (stale success lines, negative
//!   authentication, removals, CRLF, an unfinished last line), and starts over on rotation,
//!   truncation and a rewrite; the allowlist's quirks.
//! - Health: the `SFN1` state machine (echo, client learning, PONG matching, expiry, probe cadence,
//!   pending only on EResult 1) and the lines `report_network_health` logs.
//! - Discovery: the 32-byte mapping's layout, the publisher's magic order, the reader's checks.
//! - `connected_peers` against both pins' oracle goldens (tests/fixtures/network/peers.lua): the
//!   pregame denial the oracle's state reaches.
// Another test package may load support through other shared bodies too; each copy belongs to its own bodies.
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
mod support;

use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::availability::{FactsConfig, KilnRuntimeHost, OwnerActivity, RuntimeFacts};
use kiln_sfi::runtime::network::authorization::{LogAuth, full_scan, read_allowlist};
use kiln_sfi::runtime::network::discovery::{
    LocalServerIdentity, MAGIC, MAX_AGE_MS, accept, publish_into, read_from,
};
use kiln_sfi::runtime::network::health::{self, Health, parse_nonce};
use kiln_sfi::runtime::network::log_follow::{Follower, LINE_BYTES};
use kiln_sfi::runtime::network::{NetworkShared, NoPorts, install_into};
use kiln_sfi::runtime::operation_catalog::CATALOG;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::worker::Host;
use mlua::Table;
use serde_json::json;
use sfi_core::bindings::runtime::{RuntimeHost, register_runtime};
use sfi_core::conformance::{GoldenMatch, compare_golden, run_fixture_in_vm};
use sfi_core::host::{
    HostContext, Package as RuntimePackage, PackageSource, Phase, Side, SourceVersion,
};
use sfi_core::lua::LuaVm;
use sfi_core::package;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use support::*;

const A: u64 = 76_561_198_000_000_001;
const B: u64 = 76_561_198_000_000_002;
const C: u64 = 76_561_198_000_000_003;

fn added(key: &str, id: u64) -> String {
    format!("[12:00:00,001] [online] Added peer {key} (steamid:{id}) from 10.0.0.9:15636\n")
}
fn removed(key: &str) -> String {
    format!("[12:00:05,002] [online] Removed peer {key} reason=closed\n")
}
fn authenticated(id: u64) -> String {
    format!("[12:00:00,500] [server] Client '{id}' authenticated by steam\n")
}
fn not_authenticated(id: u64) -> String {
    format!("[12:00:00,600] [server] Client '{id}' not authenticated by steam\n")
}

/// A log with every branch of the scan: stale success lines, a negative authentication, removals,
/// a re-added key, lines that look alike but match nothing, CRLF, a NUL.
fn fixture() -> Vec<u8> {
    let mut log = String::new();
    log += "[11:59:59,000] [server] starting dedicated server\n";
    log += &added("peer#1", A);
    log += &authenticated(A);
    log += &added("peer#2", B);
    log += &not_authenticated(B);
    log += &authenticated(B);
    log += "[12:00:01,000] [server] Client '0' authenticated by steam\n";
    log += "[12:00:01,000] [online] Added peer  (steamid:76561198000000009)\n"; // empty key
    log += "[12:00:01,000] [online] Added peer peer#9\n"; // no space after the key
    log += "[12:00:01,000] [online] Added peer peer#8 no steam id here\n";
    log += "[12:00:01,000] [online] Added peer peer#7 (steamid:99999999999999999999999)\n"; // overflow
    log += &added("peer#3", C);
    log += &authenticated(C);
    log += &not_authenticated(C);
    log += "[12:00:02,000] [server] Client '76561198000000003' authenticated by steam\r\n";
    log += &removed("peer#2");
    log += "[12:00:03,000] [online] Removed peer peer#1\n"; // no space: not removed
    log += &added("peer#2", B); // the same key again
    log += "[12:00:04,000] [x] \u{0} Client '76561198000000004' authenticated by steam\n";
    log += &added("peer#4", 76_561_198_000_000_004);
    log += &removed("peer#3");
    log += &added("peer#3", C);
    log += "[12:00:06,000] [online] Added peer peer#5 (steamid:76561198000000005)"; // unfinished
    log.into_bytes()
}

fn temp(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!(
        "kiln-sfi-v4r3-{name}-{}-{}",
        std::process::id(),
        next_owner()
    ));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}
fn append(path: &Path, bytes: &[u8]) {
    std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .unwrap()
        .write_all(bytes)
        .unwrap();
}

/// ShroudForge's scan, as the expected values of the fixture say it.
pub fn the_server_log_scan_follows_shroudforge() {
    // A stays (authenticated, live under peer#1: its removal line has no space after the key);
    // B is live again under peer#2 and authenticated after its negative line; C was authenticated,
    // negatively authenticated, authenticated again (CRLF line), removed and re-added; 4 is
    // authenticated (a NUL earlier on the line) and live; 5's line is unfinished but counts.
    assert_eq!(full_scan(&fixture()), [A, B, C, 76_561_198_000_000_004]);
    let mut auth = LogAuth::default();
    auth.apply(added("k", 5).trim_end().as_bytes());
    auth.apply(authenticated(5).trim_end().as_bytes());
    assert_eq!(auth.peers(), [5]);
    // "not authenticated" is checked after "authenticated" and before "Removed peer".
    auth.apply(b"[online] Removed peer k not authenticated by steam Client '5'");
    assert_eq!(
        auth.peers(),
        [] as [u64; 0],
        "the negative branch wins, the removal is not seen"
    );
    auth.apply(authenticated(5).trim_end().as_bytes());
    assert_eq!(auth.peers(), [5]);
    // An empty log, and one with only an unfinished line.
    assert!(full_scan(b"").is_empty());
    assert!(full_scan(added("k", 6).trim_end().as_bytes()).is_empty());
    // The allowlist (read_network_allowlist): tokens split on , and ;, trimmed, 16-20 digits,
    // nonzero; duplicates and leading zeros kept; restriction on the raw trimmed text.
    let list = |text: &str| {
        read_allowlist(&json!({"modules": {"network": {"allowedClientSteamIds": text}}}))
    };
    let parsed = list(
        " 76561198000000001 ;76561198000000001, 0076561198000000002,123, 00000000000000000, x;",
    );
    assert_eq!(
        parsed.ids,
        [
            "76561198000000001",
            "76561198000000001",
            "0076561198000000002"
        ]
    );
    assert!(parsed.restrict);
    assert_eq!(parsed.native_ids(), [A, A, B]);
    assert!(
        list("123, abc").restrict,
        "an invalid-only list still restricts"
    );
    assert!(list("123, abc").ids.is_empty());
    assert!(!list(" \t ").restrict);
    assert!(!read_allowlist(&json!({})).restrict);
}

/// The follower against ShroudForge's whole-file scan after every chunk the log grows by.
pub fn the_log_follower_matches_the_full_scan_as_the_log_grows() {
    let log = fixture();
    for chunk in [1usize, 2, 3, 7, 16, 61, 4096] {
        let dir = temp("grow");
        let path = dir.join("enshrouded_server.log");
        let mut follower = Follower::new(path.clone());
        let first = follower.pass().expect("an unreadable log is published");
        assert!(!first.readable && first.peers.is_empty());
        assert!(
            follower.pass().is_none(),
            "nothing changed, nothing published"
        );
        let mut published = first;
        let mut written = 0;
        while written < log.len() {
            let end = (written + chunk).min(log.len());
            append(&path, &log[written..end]);
            written = end;
            if let Some(auth) = follower.pass() {
                assert!(auth.generation > published.generation);
                published = auth;
            }
            let expected = full_scan(&log[..written]);
            assert_eq!(follower.peers(), expected, "chunk {chunk}, {written} bytes");
            assert!(published.readable);
            assert_eq!(
                published.peers, expected,
                "published, chunk {chunk}, {written} bytes"
            );
        }
        assert_eq!(follower.rescans(), 1, "one read from byte 0, chunk {chunk}");
        let _ = std::fs::remove_dir_all(&dir);
    }
}

/// Rotation, truncation, a rewrite in place, an unreadable log, an over-long line.
pub fn the_log_follower_starts_over_when_the_log_is_replaced() {
    let dir = temp("rotate");
    let path = dir.join("enshrouded_server.log");
    let mut follower = Follower::new(path.clone());
    let first = [
        added("p1", A),
        authenticated(A),
        added("p2", B),
        authenticated(B),
    ]
    .concat();
    append(&path, first.as_bytes());
    assert_eq!(follower.pass().unwrap().peers, [A, B]);
    // Rotation by rename: the new file is read from byte 0, the old state is gone.
    std::fs::rename(&path, dir.join("enshrouded_server.log.1")).unwrap();
    let second = [added("q1", C), authenticated(C)].concat();
    append(&path, second.as_bytes());
    assert_eq!(follower.pass().unwrap().peers, [C]);
    assert_eq!(follower.rescans(), 2);
    // Rotation to a file with the same first bytes, longer than what was read: only the file's
    // identity tells (Windows: volume and file index; elsewhere device and inode).
    let banner =
        "[11:59:59,000] [server] Enshrouded dedicated server log, the same first line every time\n";
    std::fs::write(&path, [banner, &added("s1", A), &authenticated(A)].concat()).unwrap();
    assert_eq!(follower.pass().unwrap().peers, [A]);
    let rescans = follower.rescans();
    std::fs::rename(&path, dir.join("enshrouded_server.log.2")).unwrap();
    let longer = [
        banner,
        &added("s2", B),
        &authenticated(B),
        &"[server] noise\n".repeat(8),
    ]
    .concat();
    std::fs::write(&path, longer).unwrap();
    assert_eq!(
        follower.pass().unwrap().peers,
        [B],
        "A's lines are in the old file only"
    );
    assert_eq!(follower.rescans(), rescans + 1);
    // Truncation in place (shorter than what was read).
    std::fs::write(&path, authenticated(A)).unwrap();
    assert_eq!(follower.pass().unwrap().peers, [] as [u64; 0]);
    assert_eq!(full_scan(authenticated(A).as_bytes()), [] as [u64; 0]);
    assert_eq!(follower.rescans(), rescans + 2);
    // Rewritten in place, as long as before or longer, with other first bytes.
    let other = [added("r1", B), authenticated(B), added("r2", A)].concat();
    std::fs::write(&path, &other).unwrap();
    assert_eq!(follower.pass().unwrap().peers, full_scan(other.as_bytes()));
    assert_eq!(follower.rescans(), rescans + 3);
    // Deleted: unreadable, published once, the state forgotten.
    std::fs::remove_file(&path).unwrap();
    let gone = follower.pass().unwrap();
    assert!(!gone.readable && gone.peers.is_empty());
    assert!(follower.pass().is_none());
    append(&path, first.as_bytes());
    assert_eq!(follower.pass().unwrap().peers, [A, B]);
    // A line over 64 KiB is skipped up to its line break and counted; the lines after it count.
    let long = format!(
        "[online] Added peer p9 (steamid:{C}) {}\n",
        "x".repeat(LINE_BYTES)
    );
    append(&path, long.as_bytes());
    append(&path, authenticated(C).as_bytes());
    append(&path, [added("p3", C)].concat().as_bytes());
    assert_eq!(follower.pass().unwrap().peers, [A, B, C]);
    assert_eq!(follower.ignored_lines(), 1);
    let _ = std::fs::remove_dir_all(&dir);
}

/// The `SFN1` health state machine.
pub fn the_health_check_follows_shroudforge() {
    let mut client = Health::default();
    // Echo verbatim, any bytes after the prefix, both roles; the client learns the sender.
    assert_eq!(
        client.on_message(false, A, b"SFN1:PING:\xff\x00abc", 1_000),
        Some(b"SFN1:PONG:\xff\x00abc".to_vec())
    );
    assert_eq!(client.remote_server(1_000), Some(A));
    assert_eq!(
        client.remote_server(16_000),
        Some(A),
        "15,000 ms is still fresh"
    );
    assert_eq!(client.remote_server(16_001), None);
    assert_eq!(client.remote_server(999), None, "from the future");
    assert_eq!(
        client.on_message(false, B, b"SFN1:PING:", 2_000),
        Some(b"SFN1:PONG:".to_vec())
    );
    assert_eq!(client.remote_server(2_000), Some(B), "the last sender");
    // Sizes 1..128 only; a client ignores PONGs; other text is ignored.
    let big = [b"SFN1:PING:".as_slice(), &[b'9'; 119]].concat();
    assert_eq!(big.len(), 129);
    assert_eq!(client.on_message(false, C, &big, 3_000), None);
    assert_eq!(
        client
            .on_message(false, C, &big[..128], 3_000)
            .map(|r| r.len()),
        Some(128)
    );
    assert_eq!(client.on_message(false, C, b"", 3_000), None);
    assert_eq!(client.on_message(false, C, b"SFN1:PONG:1", 3_000), None);
    assert_eq!(client.on_message(false, C, b"sfn1:ping:1", 3_000), None);
    assert!(!client.probe_due(false, 1_000_000), "a client never probes");

    let mut server = Health::default();
    assert!(
        !server.probe_due(true, 4_999),
        "the first round once 5,000 ms have passed"
    );
    assert!(server.probe_due(true, 5_000));
    assert!(!server.probe_due(true, 9_999));
    let (n1, n2, n3) = (
        server.next_nonce(),
        server.next_nonce(),
        server.next_nonce(),
    );
    assert_eq!((n1, n2, n3), (1, 2, 3));
    server.probed(n1, A, 5_000, 1);
    server.probed(n2, B, 5_000, 25);
    server.probed(n3, C, 5_000, 1);
    assert_eq!(server.pending().len(), 2, "pending only on EResult 1");
    assert_eq!(
        (
            server.stats().probes_sent,
            server.stats().probe_send_failures,
            server.stats().last_send_result
        ),
        (2, 1, 1)
    );
    // A server learns no remote server from a PING, and answers it.
    assert_eq!(
        server.on_message(true, A, b"SFN1:PING:7", 5_100),
        Some(b"SFN1:PONG:7".to_vec())
    );
    assert_eq!(server.remote_server(5_100), None);
    // PONG matching: the whole suffix a u64, the nonce pending, the same peer.
    for bad in [
        &b"SFN1:PONG:+1"[..],
        b"SFN1:PONG: 1",
        b"SFN1:PONG:1 ",
        b"SFN1:PONG:",
        b"SFN1:PONG:0x1",
    ] {
        assert_eq!(server.on_message(true, A, bad, 5_200), None);
    }
    assert_eq!(
        server.on_message(true, B, b"SFN1:PONG:1", 5_200),
        None,
        "another peer"
    );
    assert_eq!(server.stats().probes_acknowledged, 0);
    assert_eq!(server.on_message(true, A, b"SFN1:PONG:0001", 5_250), None);
    assert_eq!(
        (
            server.stats().probes_acknowledged,
            server.stats().last_acknowledged_peer,
            server.stats().last_round_trip_ms
        ),
        (1, A, 250)
    );
    assert_eq!(server.on_message(true, A, b"SFN1:PONG:1", 5_300), None);
    assert_eq!(
        server.stats().probes_acknowledged,
        1,
        "a duplicate is ignored"
    );
    assert_eq!(parse_nonce(b"18446744073709551615"), Some(u64::MAX));
    assert_eq!(parse_nonce(b"18446744073709551616"), None);
    // Expiry strictly after 15,000 ms.
    server.expire(20_000);
    assert_eq!(server.pending().len(), 1);
    server.expire(20_001);
    assert!(server.pending().is_empty());
    assert_eq!(server.stats().probe_timeouts, 1);
    // The log lines, as report_network_health words them.
    let mut lines = Health::default();
    let texts = |lines: Vec<(i32, String)>| {
        lines
            .into_iter()
            .map(|(l, t)| format!("{l}:{t}"))
            .collect::<Vec<_>>()
    };
    assert_eq!(
        texts(lines.report(false, false, true, 1)),
        [format!(
            "{KILN_LOG_WARN}:shroudforge::network: Network service is waiting for Steam Networking Messages (available=false, service_ready=false)"
        )]
    );
    assert!(
        lines.report(false, false, true, 2).is_empty(),
        "only on a change"
    );
    assert_eq!(
        texts(lines.report(true, true, true, 3)),
        [format!(
            "{KILN_LOG_INFO}:shroudforge::network: Network service is ready. Automatic Steam session acceptance and authenticated-peer health probes are active (role=server)"
        )]
    );
    lines.probed(1, A, 10, 8);
    lines.probed(2, A, 10, 8);
    assert_eq!(
        texts(lines.report(true, true, true, 20)),
        [format!(
            "{KILN_LOG_WARN}:shroudforge::network: Steam P2P health probe send failed. Check the peer identity and Steam session state (result=8, total_failures=2)"
        )]
    );
    lines.probed(3, A, 30, 1);
    lines.expire(15_031);
    assert!(
        lines.report(true, true, true, 15_031).is_empty(),
        "the outage is reported once"
    );
    lines.probed(4, B, 15_040, 1);
    assert_eq!(lines.on_message(true, B, b"SFN1:PONG:4", 15_060), None);
    // The first acknowledgement is always reported (previous_acks == 0), even inside an outage.
    assert_eq!(
        texts(lines.report(true, true, true, 15_061)),
        [format!(
            "{KILN_LOG_INFO}:shroudforge::network: Steam P2P health probe confirmed (peer={B}, round_trip_ms=20, total_acknowledged=1)"
        )]
    );
    // The same peer again inside the outage: nothing (not recovered, same peer).
    lines.probed(6, B, 15_070, 1);
    assert_eq!(lines.on_message(true, B, b"SFN1:PONG:6", 15_080), None);
    assert!(
        lines.report(true, true, true, 15_081).is_empty(),
        "recovery needs 15 s without a failure"
    );
    lines.probed(5, B, 30_100, 1);
    assert_eq!(lines.on_message(true, B, b"SFN1:PONG:5", 30_142), None);
    assert_eq!(
        texts(lines.report(true, true, true, 30_142)),
        [format!(
            "{KILN_LOG_INFO}:shroudforge::network: Steam P2P health probe confirmed (peer={B}, round_trip_ms=42, total_acknowledged=3)"
        )]
    );
    let mut client_lines = Health::default();
    assert_eq!(
        texts(client_lines.report(true, true, false, 1)),
        [format!(
            "{KILN_LOG_INFO}:shroudforge::network: Network service is ready. Automatic Steam session acceptance is active and remote server discovery is listening (role=client)"
        )]
    );
    assert_eq!(
        texts(client_lines.report(true, false, false, 2)),
        [format!(
            "{KILN_LOG_WARN}:shroudforge::network: Network service is waiting for Steam Networking Messages (available=true, service_ready=false)"
        )]
    );
    assert_eq!(health::CHANNEL, 65_535);
}

/// The same-PC mapping: layout, the publisher's order, the reader's checks.
pub fn the_discovery_mapping_follows_shroudforge() {
    assert_eq!(std::mem::size_of::<LocalServerIdentity>(), 32);
    assert_eq!(
        MAGIC.to_le_bytes(),
        *b"1DIDEWFS",
        "the magic as ShroudForge writes it"
    );
    let view = LocalServerIdentity::default();
    publish_into(&view, A, 10_000, 4242);
    // SAFETY: a live 32-byte record.
    let bytes: [u8; 32] = unsafe { std::mem::transmute_copy(&view) };
    assert_eq!(&bytes[0..8], &MAGIC.to_le_bytes());
    assert_eq!(&bytes[8..16], &A.to_le_bytes());
    assert_eq!(&bytes[16..24], &10_000u64.to_le_bytes());
    assert_eq!(&bytes[24..28], &4242u32.to_le_bytes());
    assert_eq!(&bytes[28..32], &32u32.to_le_bytes());
    // SAFETY: the record above.
    let read = |now| unsafe { read_from(&view, now) };
    assert_eq!(read(10_000), Some(A));
    assert_eq!(read(10_000 + MAX_AGE_MS), Some(A));
    assert_eq!(read(10_001 + MAX_AGE_MS), None, "older than 10 s");
    assert_eq!(read(9_999), None, "from the future");
    for (before, after, id, at, pid, size) in [
        (MAGIC, 0, A, 1, 1, 32),
        (0, MAGIC, A, 1, 1, 32),
        (MAGIC - 1, MAGIC - 1, A, 1, 1, 32),
        (MAGIC, MAGIC, 0, 1, 1, 32),
        (MAGIC, MAGIC, A, 1, 0, 32),
        (MAGIC, MAGIC, A, 1, 1, 31),
    ] {
        assert_eq!(
            accept(before, after, id, at, pid, size, 1),
            None,
            "{before} {after} {id} {pid} {size}"
        );
    }
    assert_eq!(accept(MAGIC, MAGIC, A, 1, 1, 32, 1), Some(A));
    // A reader racing the publisher only ever sees a published id, never a half-written record.
    let shared = Arc::new(LocalServerIdentity::default());
    let stop = Arc::new(AtomicBool::new(false));
    let writer = {
        let (shared, stop) = (shared.clone(), stop.clone());
        std::thread::spawn(move || {
            let mut i = 0u64;
            while !stop.load(Ordering::Relaxed) {
                let (id, pid) = if i.is_multiple_of(2) { (A, 1) } else { (B, 2) };
                publish_into(&shared, id, 5, pid);
                i += 1;
            }
        })
    };
    let mut seen = [0u32; 2];
    for _ in 0..200_000 {
        // SAFETY: a live record.
        match unsafe { read_from(&*shared, 5) } {
            Some(A) => seen[0] += 1,
            Some(B) => seen[1] += 1,
            Some(other) => panic!("a torn read accepted {other}"),
            None => {}
        }
    }
    stop.store(true, Ordering::Relaxed);
    writer.join().unwrap();
}

struct Always;
impl OwnerActivity for Always {
    fn active(&self) -> bool {
        true
    }
}
struct NoFiles;
impl PackageSource for NoFiles {
    fn read_file(&self, _: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::ErrorKind::NotFound.into())
    }
}

fn peers_golden(version: SourceVersion, golden: &str) {
    let fixture = include_str!("../fixtures/network/peers.lua");
    let owner = next_owner();
    let api: &'static KilnApi = Box::leak(api(true));
    let services = Rc::new(ServiceAdapter::new(
        // SAFETY: the fake table lives for the process.
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let facts = Rc::new(RuntimeFacts::new(
        services.clone(),
        &CATALOG,
        FactsConfig {
            global_patch: false,
            global_export: false,
            game_build: "1.0.0.0-v4r3".into(),
        },
    ));
    // The oracle's state: pregame, a server, readable default settings.
    let context = HostContext::new(Phase::Startup, Side::Server, version);
    let manifest = package::Package::parse(
        br#"{"id":"oracle","name":"sfi oracle","version":"1.0.0","capabilities":["runtime"]}"#,
        None,
        package::ParseOptions::default(),
    )
    .unwrap()
    .manifest;
    let host: Rc<dyn RuntimeHost> = Rc::new(KilnRuntimeHost::new(
        &manifest,
        &context,
        Rc::new(Always),
        facts,
    ));
    let vm = LuaVm::new(context).unwrap();
    let env = vm
        .add_package(
            RuntimePackage::new("oracle", Rc::new(NoFiles)),
            |lua, env, package| {
                let game: Table = env.raw_get("game")?;
                game.raw_set("types", lua.create_table()?)?;
                let runtime =
                    register_runtime(lua, env, package, vm.context(), &game, host.clone())?;
                let container = lua.create_table()?;
                runtime.raw_set("network", &container)?;
                install_into(
                    lua,
                    &container,
                    host.clone(),
                    services.clone(),
                    owner,
                    Rc::new(NetworkShared::new(Rc::new(NoPorts))),
                )
            },
        )
        .unwrap();
    let actual = run_fixture_in_vm(vm.lua(), fixture, |_| Ok(env)).unwrap();
    assert_eq!(
        compare_golden(golden, &actual).unwrap_or_else(|e| panic!("{version:?}: {e}")),
        GoldenMatch::MessageVerifiedV2
    );
    assert_eq!(services.counters().calls, 0, "no Kiln member was called");
}

/// `connected_peers` answers exactly ShroudForge 1.7.9's oracle transcript.
pub fn the_peers_fixture_matches_the_oracle_sf179() {
    peers_golden(
        SourceVersion::Sf179,
        include_str!("../fixtures/network/peers.sf179.golden.json"),
    );
}
/// The same against 1.8.0's.
pub fn the_peers_fixture_matches_the_oracle_sf180() {
    peers_golden(
        SourceVersion::Sf180,
        include_str!("../fixtures/network/peers.sf180.golden.json"),
    );
}
