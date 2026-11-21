// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (modules/transport is identical in both pins): modules/transport/lua/network.rs:1-411
// (the table, the denial chain, the typed closures), modules/transport/lib.rs:7-311 (the native
// results and their texts), modules/mod-runtime/lua/bindings/mod.rs:34-57 (the runtime context
// ShroudForge's mod runtime passes), modules/mod-runtime/lua/ingame/check_ingame_access.rs:386-414.
//! The `runtime.network` family core.
//!
//! ShroudForge's network API is pull-based: every function runs on the worker
//! and calls the Steam-messages lease on `kiln.net` directly through the
//! [`ServiceAdapter`]. No hop, no inbox producer, no queue Lua can see, no file
//! IO: settings come from a worker-local snapshot ([`settings`]).
//!
//! - [`install_into`] puts `status`, `send`, `receive`, `accept`, ([`mod_messages`]) `send_mod`
//!   and `receive_mod`, and ([`authorization`])
//!   `connected_peers` into the package's `runtime.network` container (the installer owns the
//!   container; the family claims the seven names, [`CLAIMS`]).
//! - The denial chain ([`Network::denied`]), in ShroudForge's order: the registrar's
//!   `runtime_denial_reason` (phase, permission, lifecycle, static support), then the settings
//!   snapshot (unreadable, disabled with one warning per mod id), then the lease's status
//!   (not initialized, session callback not registered).
//! - The lease is reached only through kiln-abi's `KilnNetApi` (its private `steam_*` members, [`l16`]):
//!   every member read is checked against the table's size at its slot (128..176); a 128-byte
//!   `kiln.net` has none of them ([`Unavailable`](super::services::Unavailable)).
//!   A missing member, UNAVAILABLE and the other codes map to ShroudForge's texts per
//!   operation (the result table in [`raw`]).
//! - The provider facts `NetworkAvailable` (the lease's AVAILABLE) and `NetworkServiceReady` (REGISTERED)
//!   are read through [`read_available`] / [`read_service_ready`] (operation_catalog.rs).
//! - [`NetworkPorts`]: what [`maintenance`] provides (the authorized server peers, the
//!   discovered dedicated-server ids, the connected peers). [`NoPorts`] is empty: a server drops
//!   every received message and refuses every accept, as ShroudForge does with an empty authorized
//!   snapshot.
//!
//! The host maintenance ([`maintenance`]: the startup and 1 s passes, the session
//! policy, closing the lease), the health check ([`health`]), server authorization from the
//! server log ([`authorization`], followed on kiln-sfi's IO thread by [`log_follow`]),
//! `connected_peers` and same-PC discovery ([`discovery`]).

pub mod authorization;
pub mod discovery;
pub mod health;
pub mod log_follow;
pub mod maintenance;
pub mod mod_messages;
pub mod options;
pub mod raw;
pub mod settings;
pub mod status;

use super::context::RuntimeContext;
use super::install::{Family, RuntimeHook};
use super::services::{ServiceAdapter, ServiceError, ServiceTable};
use crate::service_member;
use kiln_abi::services::kiln_net::{KILN_NET_SERVICE, KILN_NET_VERSION};
use kiln_abi::{KILN_ERR_UNAVAILABLE, KILN_LOG_WARN, KILN_OK, KilnHandle, KilnOwner};
use mlua::{Lua, Table};
use settings::SettingsSnapshot;
use sfi_core::bindings::runtime::{
    RuntimeHost, RuntimeOperation as Op, RuntimeProviderFacts, runtime_denial_reason,
};
use sfi_core::host::Side;
use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::rc::Rc;
use std::sync::{Arc, Mutex, PoisonError};

/// The names this family adds inside the installer's `runtime.network` container.
pub const CLAIMS: [&str; 7] = [
    "runtime.network.status",
    "runtime.network.send",
    "runtime.network.receive",
    "runtime.network.accept",
    "runtime.network.send_mod",
    "runtime.network.receive_mod",
    "runtime.network.connected_peers",
];

/// The kiln.caps row of the Steam-messages lease (reported by kiln-net).
pub const CAPABILITY: &str = "net.steam_messages";

/// ShroudForge's texts (network.rs, lib.rs).
pub mod text {
    pub const SETTINGS_UNAVAILABLE: &str = "Network module settings are unavailable";
    pub const DISABLED: &str = "Network module is disabled in Settings. The networking service is unavailable until it is enabled.";
    /// The warning's message (tracing target `shroudforge::network`), without the final period.
    pub const DISABLED_WARNING: &str = "Network module is disabled in Settings. The networking service is unavailable until it is enabled";
    pub const NOT_INITIALIZED: &str =
        "Steam Networking Messages is not initialized in this process";
    pub const NOT_REGISTERED: &str = "Steam Network session callback could not be registered";
    pub const MODULE_UNAVAILABLE: &str = "Steam Networking runtime module is unavailable";
    pub const INTERFACE_UNAVAILABLE: &str = "Steam Networking Messages interface is unavailable";
    pub const RECEIVE_FAILED: &str =
        "Steam Networking receive failed or returned an invalid message";
    pub const NOT_AUTHORIZED: &str =
        "peer is not a currently authenticated and Network-authorized server client";
    pub const NO_PENDING_SESSION: &str = "no pending Steam P2P session for this peer";
}

/// The private Steam-messages block of `kiln.net` (`kiln_net.h`'s internal block, members
/// `steam_*` at 128..176 of a 184-byte `KilnNetApi`), from kiln-abi. The flag constants are
/// generated as `i32` enums; the `u32` copies here are what the records' fields carry.
pub mod l16 {
    use kiln_abi::services::kiln_net as abi;
    pub use kiln_abi::services::kiln_net::{
        KILN_STEAM_ERR_INVALID_DATA, KILN_STEAM_MESSAGE_MAX, KilnNetApi, KilnSteamAcceptPolicy,
        KilnSteamMessageInfo, KilnSteamMessagesOpen, KilnSteamMessagesStatus,
    };

    pub const KILN_STEAM_MSG_AVAILABLE: u32 = abi::KILN_STEAM_MSG_AVAILABLE as u32;
    pub const KILN_STEAM_MSG_SERVER_IFACE: u32 = abi::KILN_STEAM_MSG_SERVER_IFACE as u32;
    pub const KILN_STEAM_MSG_IDENTITY: u32 = abi::KILN_STEAM_MSG_IDENTITY as u32;
    pub const KILN_STEAM_MSG_REGISTERED: u32 = abi::KILN_STEAM_MSG_REGISTERED as u32;
    pub const KILN_STEAM_MSG_LEASED: u32 = abi::KILN_STEAM_MSG_LEASED as u32;
    pub const KILN_STEAM_MSG_CLOSED_FOR_EXIT: u32 = abi::KILN_STEAM_MSG_CLOSED_FOR_EXIT as u32;
    pub const KILN_STEAM_OPEN_SESSIONS: u32 = abi::KILN_STEAM_OPEN_SESSIONS as u32;
    pub const KILN_STEAM_ACCEPT_NONE: u32 = abi::KILN_STEAM_ACCEPT_NONE as u32;
    pub const KILN_STEAM_ACCEPT_ANY: u32 = abi::KILN_STEAM_ACCEPT_ANY as u32;
    pub const KILN_STEAM_ACCEPT_LISTED: u32 = abi::KILN_STEAM_ACCEPT_LISTED as u32;
    pub const KILN_STEAM_SEND_UNRELIABLE: u32 = abi::KILN_STEAM_SEND_UNRELIABLE as u32;
    pub const KILN_STEAM_SEND_RELIABLE: u32 = abi::KILN_STEAM_SEND_RELIABLE as u32;
    pub const KILN_STEAM_SEND_RESTART_BROKEN: u32 = abi::KILN_STEAM_SEND_RESTART_BROKEN as u32;

    /// A zeroed record with its `size` set (the records are plain C data; kiln-abi derives no
    /// `Default`).
    pub fn record<T: Copy>() -> T {
        // SAFETY: every lease record is plain integers and pointers, valid when zeroed.
        let mut value: T = unsafe { core::mem::zeroed() };
        // SAFETY: every lease record starts with its `size: u32`.
        unsafe { (&raw mut value).cast::<u32>().write(size_of::<T>() as u32) };
        value
    }

    // The layout the family relies on (kiln-abi's own layout checks cover the rest).
    const _: () = {
        use core::mem::{offset_of, size_of};
        assert!(offset_of!(KilnNetApi, steam_status) == 128);
        assert!(offset_of!(KilnNetApi, steam_accept) == 176);
        assert!(size_of::<KilnNetApi>() == 184);
    };
}
use l16::*;

/// Binds `kiln.net` v1 as the extended table: its members are read only where its size covers
/// them.
pub fn bind(services: &ServiceAdapter) -> Result<ServiceTable<KilnNetApi>, ServiceError> {
    services.bind::<KilnNetApi>(KILN_NET_SERVICE, KILN_NET_VERSION)
}

/// The lease's status now (one `status` call; never cached). `None`: no table, no member, or
/// the member refused, which is "not available" for every caller.
pub fn steam_status(services: &ServiceAdapter) -> Option<KilnSteamMessagesStatus> {
    let table = bind(services).ok()?;
    let status = service_member!(table, KilnNetApi, steam_status).ok()?;
    let mut out: KilnSteamMessagesStatus = record();
    // SAFETY: kiln_net.h steam_status: an out record with its size set.
    let result = unsafe { services.call(status, (&raw mut out,)) };
    (result == KILN_OK).then_some(out)
}

/// The probe behind the network operations' static support: `kiln.net` with the lease's
/// `status` member (slot 128). The other members are checked where they are called.
pub fn probe(services: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = bind(services)?;
    service_member!(table, KilnNetApi, steam_status).map(drop)
}

/// Provider fact `NetworkAvailable`: the lease's AVAILABLE flag now.
pub fn read_available(services: &ServiceAdapter, facts: &mut RuntimeProviderFacts) {
    facts.network_available =
        steam_status(services).is_some_and(|s| s.flags & KILN_STEAM_MSG_AVAILABLE != 0);
}
/// Provider fact `NetworkServiceReady`: the lease's REGISTERED flag now.
pub fn read_service_ready(services: &ServiceAdapter, facts: &mut RuntimeProviderFacts) {
    facts.network_service_ready =
        steam_status(services).is_some_and(|s| s.flags & KILN_STEAM_MSG_REGISTERED != 0);
}

/// What the maintenance provides ([`maintenance::Maintenance`]); [`NoPorts`] is empty.
pub trait NetworkPorts {
    /// ShroudForge's `is_authorized_server_peer`: in the authenticated and allowed snapshot.
    fn authorized_server_peer(&self, peer: u64) -> bool {
        let _ = peer;
        false
    }
    /// The same-PC dedicated server (client only, the mapping, at most 10 s old).
    fn local_dedicated_server_steam_id(&self) -> Option<u64> {
        None
    }
    /// The last PING sender (client only, at most 15 s).
    fn remote_dedicated_server_steam_id(&self) -> Option<u64> {
        None
    }
    /// The dedicated server's live and authenticated peers as the log follower last read them;
    /// `None` when the log cannot be read (or nothing follows it).
    fn connected_peers(&self) -> Option<Vec<u64>> {
        None
    }
}
/// No ports (the family without its maintenance).
pub struct NoPorts;
impl NetworkPorts for NoPorts {}

/// The family's state of one runtime-create, shared by every package's closures (worker only).
pub struct NetworkShared {
    settings: SettingsSnapshot,
    ports: Rc<dyn NetworkPorts>,
    lease: Cell<Option<KilnHandle>>,
    /// The worker's copy of the lease, for the one close (`WorkerShared::network_lease`).
    lease_slot: RefCell<Option<Arc<crate::worker::LeaseSlot>>>,
    /// The one receive buffer (512 KiB, made at the first receive and kept).
    buffer: RefCell<Vec<u8>>,
}
impl NetworkShared {
    pub fn new(ports: Rc<dyn NetworkPorts>) -> Self {
        Self {
            settings: SettingsSnapshot::default(),
            ports,
            lease: Cell::new(None),
            lease_slot: RefCell::new(None),
            buffer: RefCell::new(Vec::new()),
        }
    }
    /// The loader settings snapshot (the single-key state class until CF1 / Q-N1).
    pub fn settings(&self) -> &SettingsSnapshot {
        &self.settings
    }
    pub fn ports(&self) -> &Rc<dyn NetworkPorts> {
        &self.ports
    }
    /// The open lease, if one was opened.
    pub fn lease(&self) -> Option<KilnHandle> {
        self.lease.get()
    }
    /// Hands the lease over (closed after the Lua stop, or by the tick after a fault).
    pub fn take_lease(&self) -> Option<KilnHandle> {
        self.lease.take()
    }
    /// The worker's lease slot (set at runtime-create); an open lease is copied there.
    pub fn set_lease_slot(&self, slot: Arc<crate::worker::LeaseSlot>) {
        if let Some(lease) = self.lease.get() {
            slot.hold(lease);
        }
        *self.lease_slot.borrow_mut() = Some(slot);
    }
    pub fn lease_slot(&self) -> Option<Arc<crate::worker::LeaseSlot>> {
        self.lease_slot.borrow().clone()
    }
}

/// The lease of `shared`, opened on first use: channels 0..65535, answering session requests
/// (the interpreter is the one session owner; the maintenance sets its policy). Opening calls no
/// Steam.
pub fn open_lease(
    services: &ServiceAdapter,
    owner: KilnOwner,
    shared: &NetworkShared,
) -> Result<KilnHandle, CallError> {
    if let Some(lease) = shared.lease.get() {
        return Ok(lease);
    }
    let table = bind(services).map_err(|_| CallError::Missing)?;
    let open = service_member!(table, KilnNetApi, steam_open).map_err(|_| CallError::Missing)?;
    let mut request: KilnSteamMessagesOpen = record();
    request.flags = KILN_STEAM_OPEN_SESSIONS;
    request.first_channel = 0;
    request.last_channel = 65_535;
    let mut lease: KilnHandle = 0;
    // SAFETY: kiln_net.h steam_open: a sized request record and a live out handle.
    let result = unsafe { services.call(open, (owner, &raw const request, &raw mut lease)) };
    if result != KILN_OK {
        return Err(CallError::of(result));
    }
    shared.lease.set(Some(lease));
    if let Some(slot) = shared.lease_slot.borrow().as_ref() {
        slot.hold(lease);
    }
    Ok(lease)
}

/// Why a lease call did not happen or did not succeed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallError {
    /// No `kiln.net`, a short table or a NULL member (the service adapter's `Unavailable`).
    Missing,
    /// `KILN_ERR_UNAVAILABLE` (Steam not ready, closed for exit, not the lease's owner).
    Unavailable,
    /// Any other code.
    Failed(i32),
}
impl CallError {
    fn of(result: i32) -> Self {
        if result == KILN_ERR_UNAVAILABLE {
            Self::Unavailable
        } else {
            Self::Failed(result)
        }
    }
}

/// Mod ids already warned that the network module is disabled (ShroudForge: once per mod id
/// for the process).
static WARNED: Mutex<Option<HashSet<String>>> = Mutex::new(None);

/// One package's view: its host, the adapter, kiln-sfi's owner and the shared state.
pub struct Network {
    host: Rc<dyn RuntimeHost>,
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
    shared: Rc<NetworkShared>,
}

impl Network {
    pub fn services(&self) -> &ServiceAdapter {
        &self.services
    }
    pub fn shared(&self) -> &NetworkShared {
        &self.shared
    }
    /// The package's own id.
    pub fn id(&self) -> &str {
        self.host.owner_context().id()
    }
    /// ShroudForge's `is_server` (the package's side).
    pub fn is_server(&self) -> bool {
        self.host.owner_context().side() == Side::Server
    }

    /// ShroudForge's `denied` (network.rs:57-90) for `operation`.
    pub fn denied(&self, operation: Op) -> Option<String> {
        if let Some(reason) = runtime_denial_reason(self.host.as_ref(), operation) {
            return Some(reason);
        }
        let Some(disabled) = self.shared.settings.read(settings::disabled) else {
            return Some(text::SETTINGS_UNAVAILABLE.into());
        };
        if disabled {
            self.warn_disabled(operation);
            return Some(text::DISABLED.into());
        }
        match steam_status(&self.services) {
            Some(s) if s.flags & KILN_STEAM_MSG_AVAILABLE == 0 => {
                Some(text::NOT_INITIALIZED.into())
            }
            None => Some(text::NOT_INITIALIZED.into()),
            Some(s) if s.flags & KILN_STEAM_MSG_REGISTERED == 0 => {
                Some(text::NOT_REGISTERED.into())
            }
            Some(_) => None,
        }
    }

    fn warn_disabled(&self, operation: Op) {
        let id = self.id().to_owned();
        let first = WARNED
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .get_or_insert_with(HashSet::new)
            .insert(id.clone());
        if first {
            self.services.log(
                self.owner,
                KILN_LOG_WARN,
                &format!(
                    "shroudforge::network: {} (mod_id={id}, operation={})",
                    text::DISABLED_WARNING,
                    operation.as_str()
                ),
            );
        }
    }

    /// The lease ([`open_lease`]).
    pub fn lease(&self) -> Result<KilnHandle, CallError> {
        open_lease(&self.services, self.owner, &self.shared)
    }

    /// The bound table, for one member call.
    fn table(&self) -> Result<ServiceTable<KilnNetApi>, CallError> {
        bind(&self.services).map_err(|_| CallError::Missing)
    }
}

/// Installs `status`, `send`, `receive`, `accept`, `send_mod`, `receive_mod` and
/// `connected_peers` into `network` (a package's `runtime.network` container), each closure
/// holding this package's [`Network`].
pub fn install_into(
    lua: &Lua,
    network: &Table,
    host: Rc<dyn RuntimeHost>,
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
    shared: Rc<NetworkShared>,
) -> mlua::Result<()> {
    let network_view = Rc::new(Network {
        host,
        services,
        owner,
        shared,
    });
    network.raw_set("status", status::create(lua, network_view.clone())?)?;
    network.raw_set("send", raw::send(lua, network_view.clone())?)?;
    network.raw_set("receive", raw::receive(lua, network_view.clone())?)?;
    network.raw_set("accept", raw::accept(lua, network_view.clone())?)?;
    network.raw_set(
        "send_mod",
        mod_messages::send_mod(lua, network_view.clone())?,
    )?;
    network.raw_set(
        "receive_mod",
        mod_messages::receive_mod(lua, network_view.clone())?,
    )?;
    network.raw_set(
        "connected_peers",
        authorization::connected_peers(lua, network_view)?,
    )?;
    Ok(())
}

/// The family, with this runtime-create's shared state. Without the registrar's `runtime` table (no
/// runtime host) it adds nothing.
pub fn family(shared: Rc<NetworkShared>) -> Family {
    Family::new(
        "network",
        &CLAIMS,
        move |lua: &Lua, _: &Table, _, composed, cx: &RuntimeContext| {
            let Some(runtime) = &composed.runtime else {
                return Ok(());
            };
            let network: Table = runtime.raw_get("network")?;
            let host = cx.package_host().ok_or_else(|| {
                mlua::Error::runtime("runtime.network: the package has no runtime host")
            })?;
            install_into(
                lua,
                &network,
                host,
                cx.services().clone(),
                cx.owner(),
                shared.clone(),
            )
        },
    )
}

/// The family with its maintenance (the lifecycle host's runtime hook), and both halves for
/// whoever wants to look at them.
pub fn maintained_family(
    config: maintenance::Config,
) -> (Family, Rc<maintenance::Maintenance>, Rc<NetworkShared>) {
    let maintenance = Rc::new(maintenance::Maintenance::new(config));
    let shared = Rc::new(NetworkShared::new(maintenance.clone()));
    let hook: Rc<dyn RuntimeHook> = Rc::new(maintenance::NetworkHook::new(
        shared.clone(),
        maintenance.clone(),
    ));
    (family(shared.clone()).with_hook(hook), maintenance, shared)
}

/// The production family: the network functions and their maintenance.
pub fn production_family() -> Family {
    maintained_family(maintenance::Config::production()).0
}
