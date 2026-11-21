// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (modules/transport is identical in both pins): modules/mod-runtime/ingame/
// start_ingame_runtime.rs:62-73 (the startup pass), update_ingame_runtime.rs:17-48 (the periodic pass),
// modules/transport/native/steam_network.cpp:344-429 (the service tick), :444-448 (shutdown),
// lib.rs:120-152, report_network_health.rs:35-85.
//! The network family's host maintenance: the pass ShroudForge's runtime makes once at runtime-create (before
//! any package is set up) and then at the start of a worker turn every 1 s or more, before the turn's on_update
//! dispatch ([`NetworkHook`], the lifecycle host's runtime hook).
//!
//! The pass, in ShroudForge's order (its native service tick, steam_network.cpp:344-429):
//! 1. more than 256 valid allowlist tokens: nothing at all this pass;
//! 2. the lease's status is not AVAILABLE (Steam not ready): stop (the authorized set keeps its value);
//! 3. a dedicated server publishes its Steam id for same-PC clients ([`super::discovery`]);
//! 4. a server sets its authorized set to the log follower's live and authenticated peers, kept to the allowlist when
//!    it restricts, and accepts session requests from exactly those (`maintain(LISTED)`); a client accepts any
//!    (`maintain(ANY)`). The lease (all 65,536 channels, answering session requests) is opened here first;
//! 5. the session callback is not REGISTERED: stop;
//! 6. the health channel is drained (at most 16 messages), 7. pending probes expire, 8. a server probes its peers
//!    every 5 s or more ([`super::health`]).
//!
//! The startup pass runs when `modules.network.enabled` is not `false` (missing counts as on), the periodic one only
//! when it is `true`, and is followed by the health log lines. Otherwise, or while the settings cannot be read, the
//! session policy goes to NONE (`maintain(NONE)`, in force at once in any Steam state) and every cache is kept.
//!
//! The lease is closed after the Lua stop of an orderly leave ([`NetworkHook::stopped`]), or once by kiln-sfi's tick
//! after a fault (through `WorkerShared::network_lease`). A fast leave makes no network call.
//!
//! Every call is a direct call of the lease on the worker: no hop, no wait, no file. The log follower runs on
//! kiln-sfi's IO thread ([`super::log_follow`]) and reaches the worker as `NetworkAuth` through the inbox.

use super::authorization::{Allowlist, MAX_ALLOWLIST, read_allowlist};
use super::discovery::{self, Discovery};
use super::health::{self, Health};
use super::log_follow::{FollowJob, Follower, server_log};
use super::{KilnNetApi, NetworkPorts, NetworkShared, bind, l16::*, open_lease, steam_status};
use crate::runtime::install::RuntimeHook;
use crate::runtime::{RuntimeContext, ServiceAdapter};
use crate::service_member;
use crate::state::StateHub;
use crate::worker::{NetworkAuth, WorkerCommand, WorkerContext};
use kiln_abi::{KILN_LOG_WARN, KILN_OK, KilnHandle, KilnOwner};
use serde_json::Value;
use sfi_core::host::Side;
use std::cell::{Cell, RefCell};
use std::collections::{BTreeSet, HashSet};
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

/// The periodic pass runs at most this often (`next_status`: 1 s).
pub const PERIOD_MS: u64 = 1_000;
/// The ids one session policy holds at most (kiln_net.h `KilnSteamAcceptPolicy.count`; more are refused and change
/// nothing).
pub const POLICY_IDS: usize = 256;

/// Where the server log comes from.
#[derive(Clone, Debug)]
pub enum LogSource {
    /// `<game>/logs/enshrouded_server.log`, `<game>` from the frozen launch plan (production).
    Plan,
    /// This file (tests).
    Path(PathBuf),
    /// No follower: the server's authorized set stays empty and `connected_peers` answers it cannot read the log.
    None,
}

/// What the maintenance runs on.
#[derive(Clone)]
pub struct Config {
    /// Milliseconds, GetTickCount64's in production ([`discovery::tick_ms`]).
    pub clock: Rc<dyn Fn() -> u64>,
    pub discovery: Rc<dyn Discovery>,
    pub log: LogSource,
    /// The IO thread the follower runs on; `None`: the worker's sfi.json hub (production).
    pub io: Option<Arc<StateHub>>,
}
impl Config {
    pub fn production() -> Self {
        Self {
            clock: Rc::new(discovery::tick_ms),
            discovery: discovery::production(),
            log: LogSource::Plan,
            io: None,
        }
    }
}

/// The maintenance's own state: the ports `runtime.network` reads (authorized set, discovery, connected peers).
pub struct Maintenance {
    config: Config,
    side: Cell<Option<Side>>,
    services: RefCell<Option<Rc<ServiceAdapter>>>,
    owner: Cell<KilnOwner>,
    /// The server's authorized set (`g_authorized_server_peers`).
    authorized: RefCell<BTreeSet<u64>>,
    /// The follower's latest state, and whether a follower runs at all.
    auth: RefCell<Option<NetworkAuth>>,
    following: Cell<bool>,
    /// The follower reads while this is set (the network is enabled on a server).
    active: Arc<AtomicBool>,
    health: RefCell<Health>,
    next_pass: Cell<Option<u64>>,
    passes: Cell<u64>,
}

impl Maintenance {
    pub fn new(config: Config) -> Self {
        Self {
            config,
            side: Cell::new(None),
            services: RefCell::new(None),
            owner: Cell::new(0),
            authorized: RefCell::new(BTreeSet::new()),
            auth: RefCell::new(None),
            following: Cell::new(false),
            active: Arc::new(AtomicBool::new(false)),
            health: RefCell::new(Health::default()),
            next_pass: Cell::new(None),
            passes: Cell::new(0),
        }
    }
    /// What runtime-create gives it: the adapter, kiln-sfi's owner and this process's side.
    pub fn bind(&self, services: Rc<ServiceAdapter>, owner: KilnOwner, side: Side) {
        self.side.set(Some(side));
        *self.services.borrow_mut() = Some(services);
        self.owner.set(owner);
    }
    fn now(&self) -> u64 {
        (self.config.clock)()
    }
    fn server(&self) -> bool {
        self.side.get() == Some(Side::Server)
    }
    fn client(&self) -> bool {
        self.side.get() == Some(Side::Client)
    }
    /// The health check's state (tests, status).
    pub fn health(&self) -> std::cell::Ref<'_, Health> {
        self.health.borrow()
    }
    /// The server's authorized set now.
    pub fn authorized(&self) -> Vec<u64> {
        self.authorized.borrow().iter().copied().collect()
    }
    /// Passes that got past the settings (startup and periodic).
    pub fn passes(&self) -> u64 {
        self.passes.get()
    }
    /// Whether a log follower runs (set when its IO job was added; tests set it directly).
    pub fn set_following(&self, following: bool) {
        self.following.set(following);
    }
    /// The follower reads now.
    pub fn following_active(&self) -> bool {
        self.active.load(Ordering::Acquire)
    }
    /// A follower state arrived (from the inbox; tests may give one directly).
    pub fn take_auth(&self, auth: NetworkAuth) {
        let mut held = self.auth.borrow_mut();
        if held
            .as_ref()
            .is_none_or(|held| held.generation <= auth.generation)
        {
            *held = Some(auth);
        }
    }

    /// One pass (`startup`: the one at runtime-create). Never panics on a service answer.
    pub fn pass(&self, shared: &NetworkShared, startup: bool) {
        let Some(services) = self.services.borrow().clone() else {
            return;
        };
        let enabled = shared.settings().read(|document| {
            document
                .pointer("/modules/network/enabled")
                .and_then(Value::as_bool)
                .unwrap_or(startup)
        });
        self.active
            .store(enabled == Some(true) && self.server(), Ordering::Release);
        if enabled != Some(true) {
            // ShroudForge unregisters (`service_shutdown`): the policy NONE. Nothing to do without a lease.
            if let Some(lease) = shared.lease() {
                self.maintain(&services, lease, KILN_STEAM_ACCEPT_NONE, &[]);
            }
            return;
        }
        self.passes.set(self.passes.get() + 1);
        let allowlist = shared.settings().read(read_allowlist).unwrap_or_default();
        self.tick(&services, shared, &allowlist);
        if !startup {
            let status = steam_status(&services);
            let available = status.is_some_and(|s| s.flags & KILN_STEAM_MSG_AVAILABLE != 0);
            let ready = status.is_some_and(|s| s.flags & KILN_STEAM_MSG_REGISTERED != 0);
            let lines =
                self.health
                    .borrow_mut()
                    .report(available, ready, self.server(), self.now());
            for (level, line) in lines {
                services.log(self.owner.get(), level, &line);
            }
        }
    }

    /// ShroudForge's native service tick (steam_network.cpp:344-429).
    fn tick(&self, services: &ServiceAdapter, shared: &NetworkShared, allowlist: &Allowlist) {
        let ids = allowlist.native_ids();
        if ids.len() > MAX_ALLOWLIST {
            return;
        }
        let Some(status) = steam_status(services) else {
            return;
        };
        if status.flags & KILN_STEAM_MSG_AVAILABLE == 0 {
            return;
        }
        let now = self.now();
        let server = self.server();
        if server && status.flags & KILN_STEAM_MSG_IDENTITY != 0 && status.local_steam_id != 0 {
            self.config.discovery.publish(status.local_steam_id, now);
        }
        let Ok(lease) = open_lease(services, self.owner.get(), shared) else {
            return;
        };
        let mut peers = Vec::new();
        let registered = if server {
            peers = match &*self.auth.borrow() {
                Some(auth) if auth.readable => auth.peers.clone(),
                _ => Vec::new(),
            };
            if allowlist.restrict {
                let allowed: HashSet<u64> = ids.iter().copied().collect();
                peers.retain(|peer| allowed.contains(peer));
            }
            *self.authorized.borrow_mut() = peers.iter().copied().collect();
            // ShroudForge's set has no bound; a policy holds 256 ids. Only a forged log has more peers than the
            // game's players: the first 256 (ascending) are accepted by Steam's callback, the rest only by hand
            // (`accept`), and receive and the probes keep all of them.
            self.maintain(
                services,
                lease,
                KILN_STEAM_ACCEPT_LISTED,
                &peers[..peers.len().min(POLICY_IDS)],
            )
        } else {
            self.maintain(services, lease, KILN_STEAM_ACCEPT_ANY, &[])
        };
        if !registered.is_some_and(|out| out.flags & KILN_STEAM_MSG_REGISTERED != 0) {
            return;
        }
        let mut health = self.health.borrow_mut();
        for _ in 0..health::DRAIN {
            let mut buffer = [0u8; health::MAX_PAYLOAD];
            match receive(services, lease, &mut buffer) {
                Received::Message(peer, length) => {
                    if let Some(answer) = health.on_message(server, peer, &buffer[..length], now) {
                        let result = send(services, lease, peer, &answer);
                        health.sent(result);
                    }
                }
                Received::Invalid => {}
                Received::Nothing => break,
            }
        }
        health.expire(now);
        if health.probe_due(server, now) {
            for &peer in &peers {
                let nonce = health.next_nonce();
                let ping = [health::PING, nonce.to_string().as_bytes()].concat();
                let result = send(services, lease, peer, &ping);
                health.probed(nonce, peer, now, result);
            }
        }
    }

    /// `maintain(lease, mode, ids)`: the status it answers, or `None` when it refused.
    fn maintain(
        &self,
        services: &ServiceAdapter,
        lease: KilnHandle,
        mode: u32,
        ids: &[u64],
    ) -> Option<KilnSteamMessagesStatus> {
        let table = bind(services).ok()?;
        let maintain = service_member!(table, KilnNetApi, steam_maintain).ok()?;
        let mut policy: KilnSteamAcceptPolicy = record();
        policy.mode = mode;
        policy.ids = if ids.is_empty() {
            core::ptr::null()
        } else {
            ids.as_ptr()
        };
        policy.count = ids.len() as u32;
        let mut out: KilnSteamMessagesStatus = record();
        // SAFETY: kiln_net.h steam_maintain: a sized policy whose ids live for the call, a sized out record.
        let result = unsafe { services.call(maintain, (lease, &raw const policy, &raw mut out)) };
        (result == KILN_OK).then_some(out)
    }

    /// Closes the lease once (the orderly stop, or a runtime-create that failed after the startup pass).
    pub fn close(&self, shared: &NetworkShared) {
        self.active.store(false, Ordering::Release);
        let held = shared.take_lease();
        let lease = match shared.lease_slot() {
            Some(slot) => slot.take(),
            None => held.unwrap_or(0),
        };
        let Some(services) = self.services.borrow().clone() else {
            return;
        };
        if lease == 0 {
            return;
        }
        if let Ok(table) = bind(&services)
            && let Ok(close) = service_member!(table, KilnNetApi, steam_close)
        {
            // SAFETY: kiln_net.h steam_close: a lease handle (idempotent).
            let _ = unsafe { services.call(close, (lease,)) };
        }
    }
}

impl NetworkPorts for Maintenance {
    fn authorized_server_peer(&self, peer: u64) -> bool {
        self.authorized.borrow().contains(&peer)
    }
    fn local_dedicated_server_steam_id(&self) -> Option<u64> {
        if !self.client() {
            return None;
        }
        self.config.discovery.read(self.now())
    }
    fn remote_dedicated_server_steam_id(&self) -> Option<u64> {
        if !self.client() {
            return None;
        }
        self.health.borrow().remote_server(self.now())
    }
    fn connected_peers(&self) -> Option<Vec<u64>> {
        if !self.following.get() {
            return None;
        }
        match &*self.auth.borrow() {
            // The follower has not read the log yet: nothing is known to be connected.
            None => Some(Vec::new()),
            Some(auth) if auth.readable => Some(auth.peers.clone()),
            Some(_) => None,
        }
    }
}

enum Received {
    Message(u64, usize),
    /// Taken and released: empty, too big, or without a Steam id.
    Invalid,
    /// Nothing more (or the lease refused).
    Nothing,
}

fn receive(services: &ServiceAdapter, lease: KilnHandle, buffer: &mut [u8]) -> Received {
    let Ok(table) = bind(services) else {
        return Received::Nothing;
    };
    let Ok(receive) = service_member!(table, KilnNetApi, steam_receive) else {
        return Received::Nothing;
    };
    let mut info: KilnSteamMessageInfo = record();
    let mut got = 0u32;
    // SAFETY: kiln_net.h steam_receive: `buffer` holds `cap` writable bytes, a sized info record, live outs.
    let result = unsafe {
        services.call(
            receive,
            (
                lease,
                health::CHANNEL,
                buffer.as_mut_ptr().cast(),
                buffer.len() as u32,
                &raw mut info,
                &raw mut got,
            ),
        )
    };
    match (result, got) {
        (KILN_OK, 1) if info.length as usize <= buffer.len() && info.peer_steam_id != 0 => {
            if info.length == 0 {
                Received::Invalid
            } else {
                Received::Message(info.peer_steam_id, info.length as usize)
            }
        }
        (KILN_STEAM_ERR_INVALID_DATA, _) => Received::Invalid,
        _ => Received::Nothing,
    }
}

/// One health message, reliable (ShroudForge's flags 40); Steam's EResult, -1 when the lease refused it.
fn send(services: &ServiceAdapter, lease: KilnHandle, peer: u64, bytes: &[u8]) -> i32 {
    let Ok(table) = bind(services) else {
        return -1;
    };
    let Ok(send) = service_member!(table, KilnNetApi, steam_send) else {
        return -1;
    };
    let mut steam_result = 0i32;
    // SAFETY: kiln_net.h steam_send: `bytes` readable for its length (at most 138), a live out.
    let result = unsafe {
        services.call(
            send,
            (
                lease,
                peer,
                bytes.as_ptr().cast(),
                bytes.len() as u32,
                health::CHANNEL,
                KILN_STEAM_SEND_RELIABLE | KILN_STEAM_SEND_RESTART_BROKEN,
                &raw mut steam_result,
            ),
        )
    };
    if result == KILN_OK { steam_result } else { -1 }
}

/// The lifecycle host's runtime hook for the network family: the passes, the follower's state, the lease's close.
pub struct NetworkHook {
    shared: Rc<NetworkShared>,
    maintenance: Rc<Maintenance>,
}
impl NetworkHook {
    pub fn new(shared: Rc<NetworkShared>, maintenance: Rc<Maintenance>) -> Self {
        Self {
            shared,
            maintenance,
        }
    }

    fn start_follower(&self, cx: &WorkerContext<'_>) {
        let m = &self.maintenance;
        let path = match &m.config.log {
            LogSource::Plan => cx.shared().frozen_plan().map(|plan| server_log(&plan.game)),
            LogSource::Path(path) => Some(path.clone()),
            LogSource::None => None,
        };
        let io = m
            .config
            .io
            .clone()
            .or_else(|| cx.shared().status_hub().cloned());
        let (Some(path), Some(io), Some(worker)) = (path, io, cx.shared().arc()) else {
            return;
        };
        let job = FollowJob::new(
            Follower::new(path),
            m.active.clone(),
            Arc::downgrade(&worker),
        );
        m.following.set(io.add_io_job(Box::new(job)));
        if !m.following.get() {
            cx.log(
                KILN_LOG_WARN,
                "kiln-sfi: no IO thread for the dedicated server log; runtime.network authorizes no peer",
            );
        }
    }
}

impl RuntimeHook for NetworkHook {
    fn created(&self, cx: &WorkerContext<'_>, runtime: &RuntimeContext, side: Side) {
        let m = &self.maintenance;
        m.bind(runtime.services().clone(), runtime.owner(), side);
        self.shared
            .set_lease_slot(cx.shared().network_lease().clone());
        if side == Side::Server {
            self.start_follower(cx);
        }
        m.pass(&self.shared, true);
    }
    fn command(&self, _: &WorkerContext<'_>, command: &WorkerCommand) {
        if let WorkerCommand::NetworkAuth(auth) = command {
            self.maintenance.take_auth(auth.clone());
        }
    }
    fn turn(&self, _: &WorkerContext<'_>) {
        let m = &self.maintenance;
        let now = m.now();
        if m.next_pass.get().is_some_and(|next| now < next) {
            return;
        }
        m.pass(&self.shared, false);
        m.next_pass.set(Some(m.now().saturating_add(PERIOD_MS)));
    }
    fn stopped(&self, _: &WorkerContext<'_>) {
        self.maintenance.close(&self.shared);
    }
}
