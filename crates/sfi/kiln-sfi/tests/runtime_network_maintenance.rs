// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family tests.
//! The network maintenance (startup and periodic passes, health channel, session policy,
//! authorization, discovery) and `connected_peers` on the real service adapter, the registrar's production
//! host and the C-ABI fake kiln.net, whose `maintain` and `close` follow kiln.net's real members; plus
//! the pass order against on_update, the orderly close, the fast leave and the close after a fault
//! through the real worker and lifecycle host. Pure parts and the goldens are in
//! runtime_network_maintenance_proof.rs.
//! Expected behaviour is ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), both pins:
//! modules/transport/native/steam_network.cpp:19-68,284-429,444-514, lua/network.rs:259-303,
//! modules/mod-runtime/ingame/start_ingame_runtime.rs:62-73, update_ingame_runtime.rs:17-48.
#[path = "support/fake_services/mod.rs"]
mod fake_services;
mod support;

use fake_services::Inbound;
use kiln_abi::services::kiln_caps::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::availability::{FactsConfig, KilnRuntimeHost, OwnerActivity, RuntimeFacts};
use kiln_sfi::runtime::network::authorization::{SERVER_ONLY, UNREADABLE};
use kiln_sfi::runtime::network::discovery::{
    Discovery, LocalServerIdentity, publish_into, read_from,
};
use kiln_sfi::runtime::network::l16::*;
use kiln_sfi::runtime::network::maintenance::{Config, LogSource, Maintenance};
use kiln_sfi::runtime::network::{self, CAPABILITY, NetworkShared, install_into, text};
use kiln_sfi::runtime::operation_catalog::CATALOG;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::{Families, Family};
use kiln_sfi::state::{IoJob, IoNext, StateHub};
use kiln_sfi::worker::compose::ComposePlan;
use kiln_sfi::worker::*;
use mlua::{FromLuaMulti, Lua, Table};
use serde_json::{Value, json};
use sfi_core::bindings::runtime::{RuntimeHost, register_runtime};
use sfi_core::compose::GameTypes;
use sfi_core::host::{
    HostContext, Package as RuntimePackage, PackageSource, Phase, Side, SourceVersion,
};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
use sfi_core::lua::LuaVm;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
};
use std::cell::Cell;
use std::collections::HashMap;
use std::io::Write;
use std::rc::Rc;
use std::sync::{LazyLock, Mutex, MutexGuard};
use std::time::Duration;
use support::*;

const BUILD: &str = "test-build";
const A: u64 = 76_561_198_000_000_001;
const B: u64 = 76_561_198_000_000_002;
const C: u64 = 76_561_198_000_000_003;
const SERVER_ID: u64 = 90_071_992_547_409_921;
const WAIT: Duration = Duration::from_secs(300);
const CHANNEL: u32 = 65_535;

/// kiln.caps rows are process-wide; the tests of this binary take turns.
static SERIAL: Mutex<()> = Mutex::new(());
fn serial() -> MutexGuard<'static, ()> {
    let guard = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    fake_services::set_cap(CAPABILITY, Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)));
    guard
}

struct Active;
impl OwnerActivity for Active {
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

/// The same-PC mapping in memory, with the production publisher and reader.
#[derive(Default)]
struct Mapping {
    view: LocalServerIdentity,
    publishes: Cell<u32>,
    reads: Cell<u32>,
}
impl Discovery for Mapping {
    fn publish(&self, steam_id: u64, now_ms: u64) {
        if steam_id == 0 {
            return;
        }
        self.publishes.set(self.publishes.get() + 1);
        publish_into(&self.view, steam_id, now_ms, 77);
    }
    fn read(&self, now_ms: u64) -> Option<u64> {
        self.reads.set(self.reads.get() + 1);
        // SAFETY: a live record.
        unsafe { read_from(&self.view, now_ms) }
    }
}

/// One package with the maintained network family, on this thread.
struct M {
    owner: KilnOwner,
    vm: LuaVm,
    env: Table,
    shared: Rc<NetworkShared>,
    m: Rc<Maintenance>,
    clock: Rc<Cell<u64>>,
    mapping: Rc<Mapping>,
}
fn setup(side: Side) -> M {
    let owner = next_owner();
    fake_services::bind_thread(owner);
    // Steam ready with an identity; REGISTERED follows maintain (the fake's real semantics).
    fake_services::net(owner, |n| {
        n.flags = KILN_STEAM_MSG_AVAILABLE | KILN_STEAM_MSG_IDENTITY;
        n.local_steam_id = SERVER_ID;
    });
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    let services = Rc::new(ServiceAdapter::new(
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let facts = Rc::new(RuntimeFacts::new(
        services.clone(),
        &CATALOG,
        FactsConfig {
            global_patch: false,
            global_export: false,
            game_build: BUILD.into(),
        },
    ));
    let context = HostContext::new(Phase::Runtime, side, SourceVersion::Sf179);
    let manifest = package::Package::parse(
        br#"{"id":"alpha","name":"alpha","version":"1.0.0","capabilities":["runtime"]}"#,
        None,
        package::ParseOptions::default(),
    )
    .unwrap()
    .manifest;
    let host: Rc<dyn RuntimeHost> = Rc::new(KilnRuntimeHost::new(
        &manifest,
        &context,
        Rc::new(Active),
        facts,
    ));
    let clock = Rc::new(Cell::new(100_000u64));
    let mapping = Rc::new(Mapping::default());
    let config = Config {
        clock: {
            let clock = clock.clone();
            Rc::new(move || clock.get())
        },
        discovery: mapping.clone(),
        log: LogSource::None,
        io: None,
    };
    let (_, m, shared) = network::maintained_family(config);
    m.bind(services.clone(), owner, side);
    let vm = LuaVm::new(context).unwrap();
    let env = {
        let (host, services, shared) = (host.clone(), services.clone(), shared.clone());
        vm.add_package(
            RuntimePackage::new("alpha", Rc::new(NoFiles)),
            |lua, env, package| {
                let game: Table = env.raw_get("game")?;
                game.raw_set("types", lua.create_table()?)?;
                let runtime =
                    register_runtime(lua, env, package, vm.context(), &game, host.clone())?;
                let network = lua.create_table()?;
                runtime.raw_set("network", &network)?;
                install_into(
                    lua,
                    &network,
                    host.clone(),
                    services.clone(),
                    owner,
                    shared.clone(),
                )
            },
        )
        .unwrap()
    };
    M {
        owner,
        vm,
        env,
        shared,
        m,
        clock,
        mapping,
    }
}
impl M {
    fn eval<T: FromLuaMulti>(&self, source: &str) -> T {
        self.vm
            .lua()
            .load(source)
            .set_environment(self.env.clone())
            .eval()
            .unwrap()
    }
    fn net<R>(&self, f: impl FnOnce(&mut fake_services::NetFake) -> R) -> R {
        fake_services::net(self.owner, f)
    }
    fn calls(&self, member: &str) -> u64 {
        fake_services::counters(self.owner).calls(member)
    }
    fn net_calls(&self) -> u64 {
        [
            "net.status",
            "net.open",
            "net.maintain",
            "net.send",
            "net.receive",
            "net.accept",
            "net.close",
        ]
        .iter()
        .map(|m| self.calls(m))
        .sum()
    }
    fn settings(&self, network: Value) {
        self.shared
            .settings()
            .replace(Ok(json!({"modules": {"network": network}})));
    }
    fn auth(&self, generation: u64, readable: bool, peers: &[u64]) {
        self.m.take_auth(NetworkAuth {
            generation,
            readable,
            peers: peers.to_vec(),
        });
    }
    fn advance(&self, ms: u64) {
        self.clock.set(self.clock.get() + ms);
    }
    fn queue(&self, steps: impl IntoIterator<Item = Inbound>) {
        fake_services::queue(self.owner, CHANNEL, steps);
    }
    fn health_sent(&self) -> Vec<(u64, Vec<u8>, u32)> {
        self.net(|n| {
            n.sent
                .iter()
                .filter(|s| s.channel == CHANNEL)
                .map(|s| (s.peer, s.data.clone(), s.flags))
                .collect()
        })
    }
    fn logs(&self) -> Vec<String> {
        logs(self.owner).into_iter().map(|(_, t)| t).collect()
    }
}
fn message(peer: u64, data: &[u8]) -> Inbound {
    Inbound::Message {
        peer,
        data: data.to_vec(),
        reliable: true,
    }
}

// ---- the pass ----

#[test]
fn the_startup_pass_sets_each_roles_session_policy_and_the_server_publishes_its_id() {
    let _serial = serial();
    // A server: the follower's live and authenticated peers, kept to a restricting allowlist.
    let s = setup(Side::Server);
    s.settings(json!({"enabled": true, "allowedClientSteamIds": format!("{A}; 00{B}")}));
    s.auth(1, true, &[A, B, C]);
    s.m.pass(&s.shared, true);
    assert_eq!(
        s.m.authorized(),
        [A, B],
        "leading zeros parse to the same id natively"
    );
    assert_eq!(
        s.net(|n| n.policies.clone()),
        [(KILN_STEAM_ACCEPT_LISTED, vec![A, B])]
    );
    assert_eq!(s.net(|n| (n.leases.len(), n.registrations)), (1, 1));
    assert_eq!((s.mapping.publishes.get(), s.mapping.reads.get()), (1, 0));
    // SAFETY: the record in memory.
    assert_eq!(
        unsafe { read_from(&s.mapping.view, 100_000) },
        Some(SERVER_ID)
    );
    // The startup pass logs nothing (report_network_health runs only periodically).
    assert!(
        s.logs().iter().all(|l| !l.contains("shroudforge::network")),
        "{:?}",
        s.logs()
    );
    // No allowlist: every follower peer. An invalid-only list: nobody.
    for (list, expected) in [("", vec![A, B, C]), ("123, x", vec![])] {
        s.settings(json!({"enabled": true, "allowedClientSteamIds": list}));
        s.m.pass(&s.shared, true);
        assert_eq!(s.m.authorized(), expected, "{list:?}");
        assert_eq!(
            s.net(|n| n.policy.clone()),
            Some((KILN_STEAM_ACCEPT_LISTED, expected))
        );
    }
    // An unreadable log authorizes nobody; the follower's newest generation wins.
    s.auth(3, false, &[]);
    s.auth(2, true, &[A]);
    s.settings(json!({"enabled": true}));
    s.m.pass(&s.shared, true);
    assert_eq!(s.m.authorized(), [] as [u64; 0]);
    // A client: any peer, no publish; `enabled` missing counts as on at startup only.
    let c = setup(Side::Client);
    c.settings(json!({}));
    c.m.pass(&c.shared, true);
    assert_eq!(
        c.net(|n| n.policies.clone()),
        [(KILN_STEAM_ACCEPT_ANY, vec![])]
    );
    assert_eq!(c.mapping.publishes.get(), 0);
    c.m.pass(&c.shared, false);
    assert_eq!(
        c.net(|n| n.policy.clone()),
        Some((KILN_STEAM_ACCEPT_NONE, vec![])),
        "the periodic pass needs enabled = true"
    );
}

#[test]
fn the_pass_stops_where_shroudforges_tick_stops() {
    let _serial = serial();
    let s = setup(Side::Server);
    s.auth(1, true, &[A]);
    s.m.pass(&s.shared, true);
    assert_eq!(s.m.authorized(), [A]);
    // More than 256 valid tokens: nothing at all, no Kiln call.
    let many: Vec<String> = (0..257).map(|i| (A + i).to_string()).collect();
    s.settings(json!({"enabled": true, "allowedClientSteamIds": many.join(",")}));
    s.auth(2, true, &[B]);
    let before = s.net_calls();
    s.m.pass(&s.shared, true);
    assert_eq!(s.net_calls(), before, "257 tokens skip the pass");
    assert_eq!(
        s.m.authorized(),
        [A],
        "and the authorized set keeps its value"
    );
    let exactly: Vec<String> = (0..256).map(|i| (A + i).to_string()).collect();
    s.settings(json!({"enabled": true, "allowedClientSteamIds": exactly.join(",")}));
    s.m.pass(&s.shared, true);
    assert_eq!(s.m.authorized(), [B], "256 tokens are a normal pass");
    // Steam not ready: the status only; nothing opened or maintained, the set unchanged.
    s.settings(json!({"enabled": true}));
    s.auth(3, true, &[C]);
    s.net(|n| n.flags = KILN_STEAM_MSG_IDENTITY);
    let (maintains, publishes) = (s.calls("net.maintain"), s.mapping.publishes.get());
    s.m.pass(&s.shared, true);
    assert_eq!(s.calls("net.maintain"), maintains);
    assert_eq!(
        s.mapping.publishes.get(),
        publishes,
        "no publish before Steam is ready"
    );
    assert_eq!(s.m.authorized(), [B]);
    // Not REGISTERED (the callback could not be registered): no health channel.
    let r = setup(Side::Server);
    r.net(|n| n.register_fails = true);
    r.queue([message(A, b"SFN1:PING:1")]);
    r.m.pass(&r.shared, true);
    assert_eq!(r.calls("net.maintain"), 1);
    assert_eq!(
        r.calls("net.receive"),
        0,
        "nothing drained without the callback"
    );
    r.net(|n| n.register_fails = false);
    r.m.pass(&r.shared, true);
    assert_eq!(r.calls("net.receive"), 2, "one PING, then nothing");
    assert_eq!(r.health_sent(), [(A, b"SFN1:PONG:1".to_vec(), 40)]);
}

#[test]
fn more_authorized_peers_than_a_session_policy_holds_keep_the_pass_going() {
    let _serial = serial();
    // kiln.net's session policy holds at most 256 ids and refuses more, changing nothing;
    // ShroudForge's authorized set has no bound. Only a forged server log has more live and
    // authenticated peers than the game's players: the policy takes the first 256 (ascending),
    // the authorized set (receive, accept) and the probes keep all of them, and the pass goes on.
    let s = setup(Side::Server);
    let many: Vec<u64> = (0..300).map(|i| A + i).collect();
    s.auth(1, true, &many);
    s.m.pass(&s.shared, true);
    assert_eq!(s.m.authorized(), many);
    assert_eq!(
        s.net(|n| n.policy.clone()),
        Some((KILN_STEAM_ACCEPT_LISTED, many[..256].to_vec()))
    );
    assert_eq!(
        s.health_sent().len(),
        300,
        "every authorized peer is probed"
    );
    // Back under the bound: the whole set again.
    s.auth(2, true, &many[..3]);
    s.m.pass(&s.shared, true);
    assert_eq!(
        s.net(|n| n.policy.clone()),
        Some((KILN_STEAM_ACCEPT_LISTED, many[..3].to_vec()))
    );
}

#[test]
fn the_server_log_state_is_bounded() {
    use kiln_sfi::runtime::network::authorization::{
        LogAuth, MAX_AUTHENTICATED, MAX_LIVE_PEERS, MAX_PEER_KEY,
    };
    let added = |key: &str, id: u64| format!("[online] Added peer {key} (steamid:{id}) from x");
    let removed = |key: &str| format!("[online] Removed peer {key} left");
    let yes = |id: u64| format!("[server] Client '{id}' authenticated by steam");
    let no = |id: u64| format!("[server] Client '{id}' not authenticated by steam");
    // A key longer than MAX_PEER_KEY is no live peer (the line still takes the Added branch).
    let mut auth = LogAuth::default();
    auth.apply(yes(A).as_bytes());
    auth.apply(added(&"k".repeat(MAX_PEER_KEY + 1), A).as_bytes());
    assert!(auth.peers().is_empty(), "an over-long key");
    auth.apply(added(&"k".repeat(MAX_PEER_KEY), A).as_bytes());
    assert_eq!(auth.peers(), [A]);
    // At most MAX_LIVE_PEERS live keys: a new key past them is ignored, a known key still moves,
    // and a removal makes room again.
    let mut auth = LogAuth::default();
    let base = 1_000_000u64;
    for i in 0..MAX_LIVE_PEERS as u64 {
        auth.apply(added(&format!("p{i}"), base + i).as_bytes());
    }
    for id in [A, B, C] {
        auth.apply(yes(id).as_bytes());
    }
    auth.apply(added("extra", A).as_bytes());
    assert!(auth.peers().is_empty(), "a new key past the bound");
    auth.apply(added("p0", B).as_bytes());
    assert_eq!(auth.peers(), [B], "a known key moves");
    auth.apply(removed("p1").as_bytes());
    auth.apply(added("extra", C).as_bytes());
    assert_eq!(auth.peers(), [B, C], "room again after a removal");
    // At most MAX_AUTHENTICATED ids: a new id past them is ignored until one leaves.
    let mut auth = LogAuth::default();
    for i in 0..MAX_AUTHENTICATED as u64 {
        auth.apply(yes(base + i).as_bytes());
    }
    auth.apply(added("q", A).as_bytes());
    auth.apply(yes(A).as_bytes());
    assert!(auth.peers().is_empty(), "a new id past the bound");
    auth.apply(no(base).as_bytes());
    auth.apply(yes(A).as_bytes());
    assert_eq!(auth.peers(), [A], "room again after a negative line");
}

#[test]
fn the_health_channel_is_drained_answered_and_probed() {
    let _serial = serial();
    // A client answers each PING verbatim, at most 16 messages per pass, invalid ones counted.
    let c = setup(Side::Client);
    let mut steps = vec![
        message(SERVER_ID, b"SFN1:PING:\x00\xffz"),
        message(B, &[b'x'; 129]),
        Inbound::Result(KILN_STEAM_ERR_INVALID_DATA),
        message(B, b""),
        message(B, b"SFN1:PONG:5"),
    ];
    steps.extend((0..15).map(|i| message(C, format!("SFN1:PING:{i}").as_bytes())));
    c.queue(steps);
    c.m.pass(&c.shared, true);
    assert_eq!(c.calls("net.receive"), 16, "16 per pass");
    let sent = c.health_sent();
    assert_eq!(sent[0], (SERVER_ID, b"SFN1:PONG:\x00\xffz".to_vec(), 40));
    assert_eq!(sent.len(), 12, "1 + the 11 PINGs of the 16 taken");
    assert!(
        sent[1..].iter().all(|(peer, data, flags)| *peer == C
            && data.starts_with(b"SFN1:PONG:")
            && *flags == 40)
    );
    // The client learned its remote server from the last PING sender, for at most 15 s.
    let remote = || {
        c.eval::<Option<String>>("return runtime.network.status().remote_dedicated_server_steam_id")
    };
    assert_eq!(remote(), Some(C.to_string()));
    c.m.pass(&c.shared, true);
    assert_eq!(c.calls("net.receive"), 21, "the other 4, then nothing");
    c.advance(15_000);
    assert_eq!(remote(), Some(C.to_string()));
    c.advance(1);
    assert_eq!(remote(), None);
    // A server probes its authorized peers every 5 s or more; pending only on EResult 1; a PONG
    // from the same peer with the whole nonce acknowledges.
    let s = setup(Side::Server);
    s.auth(1, true, &[A, B]);
    s.m.pass(&s.shared, true);
    let pings = s.health_sent();
    assert_eq!(
        pings,
        [
            (A, b"SFN1:PING:1".to_vec(), 40),
            (B, b"SFN1:PING:2".to_vec(), 40)
        ]
    );
    assert_eq!(s.m.health().pending().len(), 2);
    s.advance(4_999);
    s.m.pass(&s.shared, true);
    assert_eq!(s.health_sent().len(), 2, "not yet");
    s.queue([
        message(B, b"SFN1:PONG:1"),
        message(A, b"SFN1:PONG:1"),
        message(A, b"SFN1:PONG:1"),
    ]);
    s.net(|n| n.eresult = 25);
    s.advance(1);
    s.m.pass(&s.shared, true);
    let stats = s.m.health().stats();
    assert_eq!(
        (
            stats.probes_acknowledged,
            stats.last_acknowledged_peer,
            stats.last_round_trip_ms
        ),
        (1, A, 5_000)
    );
    assert_eq!(
        (
            stats.probes_sent,
            stats.probe_send_failures,
            stats.last_send_result
        ),
        (2, 2, 25)
    );
    assert_eq!(
        s.m.health().pending().keys().copied().collect::<Vec<_>>(),
        [2],
        "nonces 3 and 4 failed"
    );
    // Nonce 2 expires once more than 15,000 ms old.
    s.net(|n| n.eresult = 1);
    s.advance(10_000);
    s.m.pass(&s.shared, true);
    assert_eq!(s.m.health().stats().probe_timeouts, 0);
    s.advance(1);
    s.m.pass(&s.shared, true);
    assert_eq!(s.m.health().stats().probe_timeouts, 1);
    // The server learns no remote server and reads no mapping.
    assert_eq!(
        s.eval::<Option<String>>(
            "return runtime.network.status().remote_dedicated_server_steam_id"
        ),
        None
    );
    assert_eq!(
        s.eval::<Option<String>>("return runtime.network.status().local_dedicated_server_steam_id"),
        None
    );
    assert_eq!(s.mapping.reads.get(), 0);
}

#[test]
fn the_periodic_pass_logs_shroudforges_health_lines() {
    let _serial = serial();
    let s = setup(Side::Server);
    s.settings(json!({"enabled": true}));
    s.net(|n| n.flags = KILN_STEAM_MSG_IDENTITY);
    s.m.pass(&s.shared, false);
    s.net(|n| n.flags = KILN_STEAM_MSG_AVAILABLE | KILN_STEAM_MSG_IDENTITY);
    s.m.pass(&s.shared, false);
    s.m.pass(&s.shared, false);
    let lines: Vec<String> = s
        .logs()
        .into_iter()
        .filter(|l| l.contains("shroudforge::network"))
        .collect();
    assert_eq!(
        lines,
        [
            "shroudforge::network: Network service is waiting for Steam Networking Messages (available=false, service_ready=false)",
            "shroudforge::network: Network service is ready. Automatic Steam session acceptance and authenticated-peer health probes are active (role=server)",
        ]
    );
}

#[test]
fn disabled_or_unreadable_settings_set_the_policy_to_none_and_keep_every_cache() {
    let _serial = serial();
    let s = setup(Side::Server);
    s.settings(json!({"enabled": true}));
    s.auth(1, true, &[A]);
    s.m.pass(&s.shared, true);
    assert_eq!(s.health_sent(), [(A, b"SFN1:PING:1".to_vec(), 40)]);
    for settings in [
        Ok(json!({"modules": {"network": {"enabled": false}}})),
        Err("unreadable".to_owned()),
    ] {
        s.shared.settings().replace(settings.clone());
        let before = (
            s.calls("net.receive"),
            s.calls("net.send"),
            s.calls("net.close"),
        );
        s.m.pass(&s.shared, false);
        assert_eq!(
            s.net(|n| n.policy.clone()),
            Some((KILN_STEAM_ACCEPT_NONE, vec![])),
            "{settings:?}"
        );
        assert_eq!(
            (
                s.calls("net.receive"),
                s.calls("net.send"),
                s.calls("net.close")
            ),
            before
        );
        assert_eq!(s.m.authorized(), [A], "kept");
        assert_eq!(s.m.health().pending().len(), 1, "kept");
        assert!(!s.m.following_active(), "the follower pauses");
        assert_eq!(s.net(|n| n.open_leases()), 1, "the lease stays");
    }
    // Enabled again: the same state goes on (the next nonce is 2).
    s.settings(json!({"enabled": true}));
    s.advance(5_000);
    s.m.pass(&s.shared, false);
    assert_eq!(
        s.net(|n| n.policy.clone()),
        Some((KILN_STEAM_ACCEPT_LISTED, vec![A]))
    );
    assert_eq!(s.health_sent().last().unwrap().1, b"SFN1:PING:2");
    // Disabled before any lease: nothing to call.
    let d = setup(Side::Client);
    d.settings(json!({"enabled": false}));
    d.m.pass(&d.shared, true);
    assert_eq!(d.net_calls(), 0);
}

#[test]
fn the_close_takes_the_lease_once_and_ends_the_policy() {
    let _serial = serial();
    let s = setup(Side::Client);
    let slot = std::sync::Arc::new(LeaseSlot::default());
    s.shared.set_lease_slot(slot.clone());
    s.m.pass(&s.shared, true);
    assert_ne!(slot.get(), 0, "the worker's slot holds the lease");
    s.m.close(&s.shared);
    s.m.close(&s.shared);
    assert_eq!(s.calls("net.close"), 1, "once");
    assert_eq!(slot.get(), 0);
    assert_eq!(
        s.net(|n| (n.open_leases(), n.policy.clone())),
        (0, Some((KILN_STEAM_ACCEPT_NONE, vec![])))
    );
    // Taken by the tick first: the orderly close calls nothing.
    let t = setup(Side::Client);
    let slot = std::sync::Arc::new(LeaseSlot::default());
    t.shared.set_lease_slot(slot.clone());
    t.m.pass(&t.shared, true);
    assert_ne!(slot.take(), 0);
    t.m.close(&t.shared);
    assert_eq!(t.calls("net.close"), 0);
}

// ---- connected_peers ----

#[test]
fn connected_peers_follows_shroudforge() {
    let _serial = serial();
    let s = setup(Side::Server);
    s.m.pass(&s.shared, true);
    let peers = |s: &M| -> (Option<Vec<String>>, Option<String>) {
        let (list, reason): (Option<Table>, Option<String>) =
            s.eval("return runtime.network.connected_peers()");
        (
            list.map(|t| t.sequence_values::<String>().map(Result::unwrap).collect()),
            reason,
        )
    };
    // No follower: the log cannot be read.
    assert_eq!(peers(&s), (None, Some(UNREADABLE.to_owned())));
    s.m.set_following(true);
    assert_eq!(peers(&s), (Some(vec![]), None), "nothing read yet");
    s.auth(1, true, &[A, B, C]);
    assert_eq!(
        peers(&s),
        (
            Some(vec![A.to_string(), B.to_string(), C.to_string()]),
            None
        )
    );
    // The allowlist filters by its literal strings, only when it has valid entries: a leading zero
    // never matches here (the native authorization parses it).
    s.settings(json!({"enabled": true, "allowedClientSteamIds": format!("{C},00{B}")}));
    assert_eq!(peers(&s), (Some(vec![C.to_string()]), None));
    s.m.pass(&s.shared, true);
    assert_eq!(s.m.authorized(), [B, C]);
    s.settings(json!({"enabled": true, "allowedClientSteamIds": "123"}));
    assert_eq!(
        peers(&s).0.unwrap().len(),
        3,
        "an invalid-only list does not filter here"
    );
    // 64 peers fit; 65 are an error.
    let many: Vec<u64> = (0..65).map(|i| A + i).collect();
    s.auth(2, true, &many[..64]);
    assert_eq!(peers(&s).0.unwrap().len(), 64);
    s.auth(3, true, &many);
    assert_eq!(peers(&s), (None, Some(UNREADABLE.to_owned())));
    s.auth(4, false, &[]);
    assert_eq!(peers(&s), (None, Some(UNREADABLE.to_owned())));
    // The denial first.
    s.settings(json!({"enabled": false}));
    assert_eq!(peers(&s), (None, Some(text::DISABLED.to_owned())));
    // A client.
    let c = setup(Side::Client);
    c.settings(json!({"enabled": true}));
    c.m.pass(&c.shared, true);
    c.m.set_following(true);
    assert_eq!(peers(&c), (None, Some(SERVER_ONLY.to_owned())));
}

#[test]
fn a_client_reads_the_same_pc_server_from_the_mapping() {
    let _serial = serial();
    let c = setup(Side::Client);
    c.settings(json!({"enabled": true}));
    c.m.pass(&c.shared, true);
    let local = || {
        c.eval::<Option<String>>("return runtime.network.status().local_dedicated_server_steam_id")
    };
    assert_eq!(local(), None);
    publish_into(&c.mapping.view, SERVER_ID, 100_000, 9);
    assert_eq!(local(), Some(SERVER_ID.to_string()));
    c.advance(10_000);
    assert_eq!(local(), Some(SERVER_ID.to_string()));
    c.advance(1);
    assert_eq!(local(), None, "older than 10 s");
    assert_eq!(c.mapping.publishes.get(), 0, "a client never publishes");
}

// ---- through the worker ----

static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
fn note(owner: KilnOwner, text: String) {
    RECORDS.lock().unwrap().entry(owner).or_default().push(text);
}
fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}
thread_local! {
    static CLOCK: Rc<Cell<u64>> = Rc::new(Cell::new(1_000_000));
}
const SOURCE: &str = r#"
local step = 0
return {
  update_interval_ms = 8,
  on_update = function()
    step = step + 1
    if server then
      local peers, why = runtime.network.connected_peers()
      record('p', peers and table.concat(peers, ',') or ('!' .. tostring(why)))
      advance(1000)
      return
    end
    local s = runtime.network.status()
    record('u', tostring(step), tostring(s.remote_dedicated_server_steam_id), tostring(maintains()))
    if step == 2 then ping() advance(1000) end
  end,
}
"#;
/// Per worker (its owner): a dedicated server with this log and IO thread, or a client.
type Setup = (
    bool,
    Option<std::path::PathBuf>,
    Option<std::sync::Arc<StateHub>>,
);
static SETUPS: LazyLock<Mutex<HashMap<KilnOwner, Setup>>> = LazyLock::new(Default::default);
/// Workers whose runtime-create fails after the families' create hooks ran (a planned package
/// that was never discovered).
static FAIL_CREATE: LazyLock<Mutex<std::collections::HashSet<KilnOwner>>> =
    LazyLock::new(Default::default);
fn setup_of(owner: KilnOwner) -> Setup {
    SETUPS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or((false, None, None))
}
struct Source;
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path == "src/mod.lua" {
            Ok(SOURCE.as_bytes().to_vec())
        } else {
            Err(std::io::ErrorKind::NotFound.into())
        }
    }
}
struct NoLocale;
impl LocaleSource for NoLocale {
    fn locale(&self) -> Option<String> {
        None
    }
}
struct NoCleanup;
impl Cleanup for NoCleanup {
    fn restore(&self, _: &str) -> Result<(), String> {
        Ok(())
    }
    fn release_input_reservations(&self, _: &str) {}
}
struct Types;
impl GameTypes for Types {
    fn types(&self, lua: &Lua, _: &RuntimePackage) -> mlua::Result<Table> {
        lua.create_table()
    }
}
struct Plan;
impl PlanSource for Plan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let owner = cx.owner();
        fake_services::bind_thread(owner);
        let server = setup_of(owner).0;
        let source: Rc<dyn PackageSource> = Rc::new(Source);
        let mut parsed = package::Package::parse(
            json!({"id": "a", "name": "a", "version": "1.0.0", "capabilities": ["runtime"]})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap();
        parsed.manifest.enabled = true;
        let mut registry = Registry::default();
        registry
            .insert(
                PackageRecord::new(
                    "a".into(),
                    parsed,
                    RuntimePackage::new("a".to_owned(), source.clone()),
                    source,
                )
                .unwrap(),
            )
            .unwrap();
        let register: Register = Box::new(move |lua, env, _, _| {
            env.set("server", server)?;
            env.set(
                "maintains",
                lua.create_function(move |_, ()| {
                    Ok(fake_services::counters(owner).calls("net.maintain"))
                })?,
            )?;
            env.set(
                "record",
                lua.create_function(move |_, parts: mlua::Variadic<String>| {
                    note(owner, parts.join("|"));
                    Ok(())
                })?,
            )?;
            env.set(
                "ping",
                lua.create_function(move |_, ()| {
                    fake_services::queue(owner, CHANNEL, [message(SERVER_ID, b"SFN1:PING:9")]);
                    Ok(())
                })?,
            )?;
            env.set(
                "advance",
                lua.create_function(|_, ms: u64| {
                    CLOCK.with(|c| c.set(c.get() + ms));
                    Ok(())
                })?,
            )
        });
        let mut order = vec!["a".to_owned()];
        if FAIL_CREATE.lock().unwrap().contains(&owner) {
            order.push("missing".into());
        }
        Ok(RuntimePlan {
            side: if server { Side::Server } else { Side::Client },
            version: SourceVersion::Sf179,
            registry,
            plan: order,
            admitted: ["a".to_owned()].into(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock: process_clock(),
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: Some(register),
            compose: ComposePlan {
                game_types: Some(Rc::new(Types) as Rc<dyn GameTypes>),
                runtime: None,
                withdrawn: Vec::new(),
            },
        })
    }
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        let (_, log, io) = setup_of(cx.owner());
        let config = Config {
            clock: CLOCK.with(|c| {
                let c = c.clone();
                Rc::new(move || c.get()) as Rc<dyn Fn() -> u64>
            }),
            discovery: Rc::new(Mapping::default()),
            log: log.map_or(LogSource::None, LogSource::Path),
            io,
        };
        let (family, _, _): (Family, _, _) = network::maintained_family(config);
        Families::new(vec![family])
    }
    fn game_build(_: &WorkerContext<'_>) -> String {
        BUILD.to_owned()
    }
}

fn start(
    pool_config: PoolConfig,
) -> (
    KilnOwner,
    std::sync::Arc<FakePool>,
    std::sync::Arc<WorkerShared>,
) {
    start_as(pool_config, (false, None, None))
}
fn start_as(
    pool_config: PoolConfig,
    setup: Setup,
) -> (
    KilnOwner,
    std::sync::Arc<FakePool>,
    std::sync::Arc<WorkerShared>,
) {
    start_with(pool_config, setup, false)
}
fn start_with(
    pool_config: PoolConfig,
    setup: Setup,
    fail_create: bool,
) -> (
    KilnOwner,
    std::sync::Arc<FakePool>,
    std::sync::Arc<WorkerShared>,
) {
    let owner = next_owner();
    SETUPS.lock().unwrap().insert(owner, setup);
    if fail_create {
        FAIL_CREATE.lock().unwrap().insert(owner);
    }
    let pool = FakePool::new(owner, pool_config);
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<Plan>)));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    (owner, pool, shared)
}

#[test]
fn the_periodic_pass_runs_before_the_turns_on_update() {
    let _serial = serial();
    let (owner, pool, shared) = start(PoolConfig::default());
    assert!(
        eventually(WAIT, || records(owner).len() >= 4),
        "{:?} {:?}",
        records(owner),
        shared.status()
    );
    shared.request_stop();
    assert!(pool.task(0).wait_finished(WAIT));
    let seen = records(owner);
    // The startup pass made the first maintain, turn 1's pass the second, before on_update 1 ran
    // (after it, on_update 1 would see 1). Turn 2 has no pass (the clock did not move). on_update 2
    // queued a PING and moved the clock by 1 s; turn 3's pass took it, and on_update 3 sees the
    // remote server and the third maintain.
    assert_eq!(
        &seen[..3],
        ["u|1|nil|2", "u|2|nil|2", &format!("u|3|{SERVER_ID}|3")],
        "{seen:?}"
    );
    assert_eq!(
        fake_services::net(owner, |n| n
            .sent
            .iter()
            .filter(|s| s.channel == CHANNEL)
            .count()),
        1
    );
    // The orderly stop closed the lease after the Lua stop.
    assert_eq!(fake_services::counters(owner).calls("net.close"), 1);
    assert_eq!(fake_services::net(owner, |n| n.open_leases()), 0);
}

#[test]
fn a_fast_leave_makes_no_close_and_the_tick_closes_once_after_a_fault() {
    let _serial = serial();
    for fault in [false, true] {
        let (owner, pool, shared) = start(PoolConfig {
            fault_done: fault,
            ..PoolConfig::default()
        });
        assert!(
            eventually(WAIT, || !records(owner).is_empty()),
            "{:?}",
            shared.status()
        );
        assert_ne!(shared.network_lease().get(), 0);
        // The pool's cancellation (exit, drain, revoke): the fast leave, no mod code, no close.
        pool.cancel(shared.task());
        assert!(pool.task(0).wait_finished(WAIT));
        assert_eq!(
            fake_services::counters(owner).calls("net.close"),
            0,
            "fault {fault}"
        );
        assert_eq!(fake_services::net(owner, |n| n.open_leases()), 1);
        fire_tick(owner);
        fire_tick(owner);
        let closes = fake_services::counters(owner).calls("net.close");
        if fault {
            assert_eq!(shared.phase(), WorkerPhase::Faulted);
            assert_eq!(closes, 1, "the tick closed it once");
            assert_eq!(fake_services::net(owner, |n| n.open_leases()), 0);
            assert_eq!(shared.network_lease().get(), 0);
        } else {
            assert_eq!(
                closes, 0,
                "a fast leave without a fault leaves it to the exit"
            );
        }
    }
}

#[test]
fn a_runtime_create_that_fails_after_the_startup_pass_closes_the_lease_once() {
    let _serial = serial();
    let (owner, pool, shared) = start_with(PoolConfig::default(), (false, None, None), true);
    assert!(pool.task(0).wait_finished(WAIT));
    assert_eq!(shared.status().reason, StopReason::CreateFailed);
    assert!(
        records(owner).is_empty(),
        "no package ran: {:?}",
        records(owner)
    );
    // The startup pass opened the lease and set the client's policy; the failed create let it go.
    assert_eq!(
        fake_services::net(owner, |n| n.policies.first().cloned()),
        Some((KILN_STEAM_ACCEPT_ANY, Vec::new()))
    );
    assert_eq!(fake_services::counters(owner).calls("net.close"), 1);
    assert_eq!(fake_services::net(owner, |n| n.open_leases()), 0);
    assert_eq!(shared.network_lease().get(), 0, "nothing left for the tick");
    fire_tick(owner);
    assert_eq!(fake_services::counters(owner).calls("net.close"), 1, "once");
}

// ---- kiln-sfi's IO thread and the log follower through the worker ----

struct Counting(std::sync::Arc<std::sync::atomic::AtomicU32>, u32);
impl IoJob for Counting {
    fn run(&mut self) -> IoNext {
        let n = self.0.fetch_add(1, std::sync::atomic::Ordering::SeqCst) + 1;
        if n >= self.1 {
            IoNext::Done
        } else {
            IoNext::After(Duration::from_millis(5))
        }
    }
}
struct Panics;
impl IoJob for Panics {
    fn run(&mut self) -> IoNext {
        std::panic::resume_unwind(Box::new("a job that fails"))
    }
}

#[test]
fn kiln_sfis_io_thread_runs_each_job_when_due_and_ends_with_the_writer() {
    use std::sync::atomic::{AtomicU32, Ordering};
    let hub = std::sync::Arc::new(StateHub::new());
    let (first, second) = (
        std::sync::Arc::new(AtomicU32::new(0)),
        std::sync::Arc::new(AtomicU32::new(0)),
    );
    assert!(hub.add_io_job(Box::new(Panics)));
    assert!(hub.add_io_job(Box::new(Counting(first.clone(), 3))));
    assert!(eventually(WAIT, || first.load(Ordering::SeqCst) == 3
        && hub.io_jobs() == 0));
    // A failed job is dropped; the thread goes on with the next one.
    assert!(hub.add_io_job(Box::new(Counting(second.clone(), 2))));
    assert!(eventually(WAIT, || second.load(Ordering::SeqCst) == 2
        && hub.io_jobs() == 0));
    assert_eq!(
        first.load(Ordering::SeqCst),
        3,
        "a finished job never runs again"
    );
    hub.end().unwrap();
    assert!(
        !hub.add_io_job(Box::new(Counting(first.clone(), 1))),
        "nothing after the end"
    );
}

#[test]
fn the_server_log_reaches_authorization_through_the_io_thread_and_the_inbox() {
    let _serial = serial();
    let dir = std::env::temp_dir().join(format!(
        "kiln-sfi-v4r3-worker-{}-{}",
        std::process::id(),
        next_owner()
    ));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let log = dir.join("enshrouded_server.log");
    std::fs::write(
        &log,
        format!(
            "[online] Added peer p1 (steamid:{A}) x\n[server] Client '{A}' authenticated by steam\n"
        ),
    )
    .unwrap();
    let hub = std::sync::Arc::new(StateHub::new());
    let (owner, pool, shared) = start_as(
        PoolConfig::default(),
        (true, Some(log.clone()), Some(hub.clone())),
    );
    let has = |line: &str| records(owner).iter().any(|r| r == line);
    assert!(
        eventually(WAIT, || has(&format!("p|{A}"))),
        "{:?} {:?}",
        records(owner),
        shared.status()
    );
    assert!(eventually(WAIT, || fake_services::net(owner, |n| n
        .policy
        .clone())
        == Some((KILN_STEAM_ACCEPT_LISTED, vec![A]))));
    // The player leaves: the follower sees the removal, the worker the new state.
    std::fs::OpenOptions::new()
        .append(true)
        .open(&log)
        .unwrap()
        .write_all(b"[online] Removed peer p1 left\n")
        .unwrap();
    assert!(eventually(WAIT, || has("p|")), "{:?}", records(owner));
    assert!(eventually(WAIT, || fake_services::net(owner, |n| n
        .policy
        .clone())
        == Some((KILN_STEAM_ACCEPT_LISTED, vec![]))));
    assert!(shared.status().inbox.delivered[CommandClass::NetworkAuth as usize] >= 2);
    shared.request_stop();
    assert!(pool.task(0).wait_finished(WAIT));
    assert!(
        eventually(WAIT, || hub.io_jobs() == 0),
        "the follower ends with the worker"
    );
    hub.end().unwrap();
    let _ = std::fs::remove_dir_all(&dir);
}
