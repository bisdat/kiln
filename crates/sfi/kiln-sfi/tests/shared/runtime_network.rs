// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family, shared bodies.
// Shared runtime.network bodies: run by kiln-sfi's own tests (tests/runtime_network_proof.rs) and by
// any other test package that includes them. Texts and orders are
// ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), both pins: modules/transport/lua/
// network.rs:57-348 (identical in 1.7.9 and 1.8.0), modules/mod-runtime/lua/bindings/mod.rs:34-57.
//! - The raw fixture (tests/fixtures/network/raw.lua) and the mod-message fixture
//!   (mod_messages.lua) against both pins' oracle goldens: the argument conversions before the
//!   denial, the denial ShroudForge's state reaches (pregame), the status table's keys. The registrar's production host, no kiln.net: nothing reaches a lease member.
//! - The production family through the real worker, lifecycle host and installer on the registrar's
//!   production host, on a core without kiln.net: the six members in each package's own
//!   container, the lifecycle denial at the top level and the catalog's unverified reason once active
//!   (send_mod and receive_mod name send's and receive's operation).
// Another test package may load support through other shared bodies too; each copy belongs to its own bodies.
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
mod support;

use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::Families;
use kiln_sfi::runtime::availability::{FactsConfig, KilnRuntimeHost, OwnerActivity, RuntimeFacts};
use kiln_sfi::runtime::network::{self, NetworkShared, NoPorts, install_into};
use kiln_sfi::runtime::operation_catalog::CATALOG;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::worker::compose::ComposePlan;
use kiln_sfi::worker::*;
use mlua::{Lua, Table};
use serde_json::json;
use sfi_core::bindings::runtime::{RuntimeHost, register_runtime};
use sfi_core::compose::GameTypes;
use sfi_core::conformance::{GoldenMatch, compare_golden, run_fixture_in_vm};
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
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{LazyLock, Mutex};
use std::time::Duration;
use support::*;

/// A liveness bound for what must happen; a passing wait returns as soon as it holds.
const WAIT: Duration = Duration::from_secs(300);
const BUILD: &str = "1.0.0.0-v4r1";
const NOT_ACTIVE: &str =
    "runtime API access is available only while this mod's lifecycle is active";

fn unverified(operation: &str) -> String {
    format!("operation {operation} has not been verified for game build {BUILD}")
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

fn golden(version: SourceVersion, fixture: &str, golden: &str) {
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
            game_build: BUILD.into(),
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

/// The raw fixture answers exactly ShroudForge 1.7.9's oracle transcript.
pub fn the_raw_fixture_matches_the_oracle_sf179() {
    golden(
        SourceVersion::Sf179,
        include_str!("../fixtures/network/raw.lua"),
        include_str!("../fixtures/network/raw.sf179.golden.json"),
    );
}
/// The same against 1.8.0's (identical transport; nothing 1.8-only is switched on).
pub fn the_raw_fixture_matches_the_oracle_sf180() {
    golden(
        SourceVersion::Sf180,
        include_str!("../fixtures/network/raw.lua"),
        include_str!("../fixtures/network/raw.sf180.golden.json"),
    );
}

/// The mod-message fixture (send_mod, receive_mod) answers exactly ShroudForge 1.7.9's
/// oracle transcript: the String/usize conversions before the denial, the denial after them.
pub fn the_mod_message_fixture_matches_the_oracle_sf179() {
    golden(
        SourceVersion::Sf179,
        include_str!("../fixtures/network/mod_messages.lua"),
        include_str!("../fixtures/network/mod_messages.sf179.golden.json"),
    );
}
/// The same against 1.8.0's.
pub fn the_mod_message_fixture_matches_the_oracle_sf180() {
    golden(
        SourceVersion::Sf180,
        include_str!("../fixtures/network/mod_messages.lua"),
        include_str!("../fixtures/network/mod_messages.sf180.golden.json"),
    );
}

// ---- a mod message between two packages over a loopback kiln.net ----

/// A loopback kiln.net (the Steam-messages members only, per kiln_net.h) and a
/// kiln.caps answering the lease's row: what one package sends with send_mod, the next
/// receive on that channel takes. Other services come from support's table.
mod loopback {
    use kiln_abi::services::kiln_caps::*;
    use kiln_abi::services::kiln_net::*;
    use kiln_abi::*;
    use std::collections::VecDeque;
    use std::ffi::c_void;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::sync::{Mutex, OnceLock};

    pub const LOCAL: u64 = 76_561_198_000_000_007;
    type Service = unsafe extern "C" fn(KilnStr, u32) -> *const c_void;
    static FALLBACK: OnceLock<Service> = OnceLock::new();
    /// One message on the wire: (channel, peer, bytes, send flags).
    pub type Sent = (u32, u64, Vec<u8>, u32);
    /// The messages sent and not yet received, oldest first.
    pub static WIRE: Mutex<VecDeque<Sent>> = Mutex::new(VecDeque::new());
    /// The owner that opened the lease: kiln.net answers the lease's other members only for it.
    static OPENER: AtomicU64 = AtomicU64::new(0);
    fn not_the_opener(owner: KilnOwner) -> bool {
        owner == 0 || owner != OPENER.load(Ordering::Relaxed)
    }

    unsafe extern "C" fn status(out: *mut KilnSteamMessagesStatus) -> KilnResult {
        // SAFETY: the caller's sized record.
        unsafe {
            (*out).flags = (KILN_STEAM_MSG_AVAILABLE
                | KILN_STEAM_MSG_REGISTERED
                | KILN_STEAM_MSG_IDENTITY) as u32;
            (*out).local_steam_id = LOCAL;
        }
        KILN_OK
    }
    unsafe extern "C" fn open(
        owner: KilnOwner,
        _: *const KilnSteamMessagesOpen,
        lease: *mut KilnHandle,
    ) -> KilnResult {
        OPENER.store(owner, Ordering::Relaxed);
        // SAFETY: the caller's out handle.
        unsafe { *lease = 1 };
        KILN_OK
    }
    #[allow(clippy::too_many_arguments)]
    unsafe extern "C" fn send(
        owner: KilnOwner,
        _: KilnHandle,
        peer: u64,
        data: *const c_void,
        len: u32,
        channel: u32,
        flags: u32,
        steam_result: *mut i32,
    ) -> KilnResult {
        if not_the_opener(owner) {
            return KILN_ERR_NOT_FOUND;
        }
        // SAFETY: the caller's bytes, readable for len.
        let bytes = unsafe { std::slice::from_raw_parts(data.cast::<u8>(), len as usize) };
        WIRE.lock()
            .unwrap()
            .push_back((channel, peer, bytes.to_vec(), flags));
        // SAFETY: the caller's out.
        unsafe { *steam_result = 1 };
        KILN_OK
    }
    unsafe extern "C" fn receive(
        owner: KilnOwner,
        _: KilnHandle,
        channel: u32,
        buffer: *mut c_void,
        cap: u32,
        out: *mut KilnSteamMessageInfo,
        got: *mut u32,
    ) -> KilnResult {
        if not_the_opener(owner) {
            return KILN_ERR_NOT_FOUND;
        }
        let mut wire = WIRE.lock().unwrap();
        // SAFETY: the caller's outs.
        unsafe { *got = 0 };
        let Some(index) = wire.iter().position(|m| m.0 == channel) else {
            return KILN_OK;
        };
        let (_, _, bytes, flags) = wire.remove(index).unwrap();
        if bytes.len() > cap as usize {
            return KILN_STEAM_ERR_INVALID_DATA;
        }
        // SAFETY: `buffer` holds `cap` bytes; the record and `got` are the caller's.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast::<u8>(), bytes.len());
            (*out).length = bytes.len() as u32;
            // The loopback's sender is this process.
            (*out).peer_steam_id = LOCAL;
            (*out).channel = channel;
            (*out).flags = flags & KILN_STEAM_SEND_RELIABLE as u32;
            *got = 1;
        }
        KILN_OK
    }
    static NET: KilnNetApi = {
        // SAFETY: every member is a nullable function pointer or plain data.
        let mut table: KilnNetApi = unsafe { std::mem::zeroed() };
        table.header = KilnServiceHeader {
            size: size_of::<KilnNetApi>() as u32,
            version: KILN_NET_VERSION,
        };
        table.steam_status = Some(status);
        table.steam_open = Some(open);
        table.steam_send = Some(send);
        table.steam_receive = Some(receive);
        table
    };
    const ROW: &str = "net.steam_messages";
    unsafe extern "C" fn find(name: KilnStr, out: *mut KilnCapRow) -> KilnResult {
        // SAFETY: the caller's name, readable for len bytes.
        let text = unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) };
        if text != ROW.as_bytes() {
            return KILN_ERR_NOT_FOUND;
        }
        // SAFETY: the caller's sized record.
        unsafe {
            (*out).state = KILN_CAP_AVAILABLE as u32;
            (*out).reason = KILN_CAP_REASON_NONE as u32;
            (*out).name = KilnStr {
                ptr: ROW.as_ptr().cast(),
                len: ROW.len(),
            };
        }
        KILN_OK
    }
    static CAPS: KilnCapsApi = {
        // SAFETY: every member is a nullable function pointer or plain data.
        let mut table: KilnCapsApi = unsafe { std::mem::zeroed() };
        table.header = KilnServiceHeader {
            size: size_of::<KilnCapsApi>() as u32,
            version: KILN_CAPS_VERSION,
        };
        table.find = Some(find);
        table
    };
    unsafe extern "C" fn service(name: KilnStr, version: u32) -> *const c_void {
        // SAFETY: the caller's name, readable for len bytes.
        match unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) } {
            b"kiln.net" => (&raw const NET).cast(),
            b"kiln.caps" => (&raw const CAPS).cast(),
            // SAFETY: support's own lookup, same contract.
            _ => unsafe { FALLBACK.get().unwrap()(name, version) },
        }
    }
    /// support's table with this kiln.net and kiln.caps.
    pub fn api() -> Box<KilnApi> {
        let mut api = super::support::api(true);
        FALLBACK.get_or_init(|| api.service.unwrap());
        api.service = Some(service);
        api
    }
}

static LOOPBACK: Mutex<()> = Mutex::new(());

fn mod_message_loopback(version: SourceVersion) {
    let _one = LOOPBACK.lock().unwrap_or_else(|e| e.into_inner());
    loopback::WIRE.lock().unwrap().clear();
    let api: &'static KilnApi = Box::leak(loopback::api());
    let services = Rc::new(ServiceAdapter::new(
        // SAFETY: the table lives for the process.
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
    // Two client packages of one runtime-create, ingame and active.
    let context = HostContext::new(Phase::Runtime, Side::Client, version);
    let vm = LuaVm::new(context).unwrap();
    let shared = Rc::new(NetworkShared::new(Rc::new(NoPorts)));
    let owner = next_owner();
    let mut envs = Vec::new();
    for id in ["sender", "world-editor"] {
        let manifest = package::Package::parse(
            json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap()
        .manifest;
        let host: Rc<dyn RuntimeHost> = Rc::new(KilnRuntimeHost::new(
            &manifest,
            vm.context(),
            Rc::new(Always),
            facts.clone(),
        ));
        let (services, shared) = (services.clone(), shared.clone());
        envs.push(
            vm.add_package(
                RuntimePackage::new(id, Rc::new(NoFiles)),
                |lua, env, package| {
                    let game: Table = env.raw_get("game")?;
                    game.raw_set("types", lua.create_table()?)?;
                    let runtime =
                        register_runtime(lua, env, package, vm.context(), &game, host.clone())?;
                    let container = lua.create_table()?;
                    runtime.raw_set("network", &container)?;
                    install_into(lua, &container, host.clone(), services, owner, shared)
                },
            )
            .unwrap(),
        );
    }
    let run = |env: &Table, source: &str| -> String {
        vm.lua()
            .load(source)
            .set_environment(env.clone())
            .eval::<String>()
            .unwrap()
    };
    let peer = loopback::LOCAL.to_string();
    // The sender's two messages: one for world-editor, one for another id on another channel.
    assert_eq!(
        run(
            &envs[0],
            &format!(
                "local a, b = runtime.network.send_mod('{peer}', 'world-editor', 'hello \\0 \\195\\169', {{ reliable = false }})
                 local c, d = runtime.network.send_mod('{peer}', 'client-addon', 'other')
                 return tostring(a) .. tostring(b) .. tostring(c) .. tostring(d)"
            )
        ),
        "truenil".repeat(2)
    );
    {
        let wire = loopback::WIRE.lock().unwrap();
        let sent: Vec<(u32, Vec<u8>, u32)> = wire
            .iter()
            .map(|(channel, _, bytes, flags)| (*channel, bytes.clone(), *flags))
            .collect();
        assert_eq!(
            sent,
            [
                (
                    34_672,
                    b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"sender\",\"to_mod\":\"world-editor\",\"payload\":\"hello \\u0000 \xc3\xa9\"}".to_vec(),
                    32
                ),
                (
                    32_262,
                    b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"sender\",\"to_mod\":\"client-addon\",\"payload\":\"other\"}".to_vec(),
                    40
                ),
            ]
        );
    }
    // world-editor polls its own channel only: one RuntimeModNetworkMessage, every field.
    let received = run(
        &envs[1],
        "local list, why = runtime.network.receive_mod()
         local out = { tostring(#list), tostring(why) }
         for _, m in ipairs(list) do
           local keys = {}
           for k in pairs(m) do keys[#keys + 1] = k end
           table.sort(keys)
           out[#out + 1] = table.concat(keys, ',')
           out[#out + 1] = m.peer_steam_id .. '|' .. m.from_mod .. '|' .. m.to_mod .. '|'
             .. string.format('%q', m.payload) .. '|' .. tostring(m.reliable)
         end
         return table.concat(out, ';')",
    );
    assert_eq!(
        received,
        format!(
            "1;nil;from_mod,payload,peer_steam_id,reliable,to_mod;{peer}|sender|world-editor|\"hello \\0 \u{e9}\"|false"
        )
    );
    assert_eq!(
        loopback::WIRE.lock().unwrap().len(),
        1,
        "the other channel's message is left alone"
    );
}

/// One package's send_mod reaches another package's receive_mod through the production
/// family, the registrar's host and the service adapter, over a loopback kiln.net (ShroudForge 1.7.9).
pub fn a_mod_message_goes_from_one_package_to_another_sf179() {
    mod_message_loopback(SourceVersion::Sf179);
}
/// The same on 1.8.0.
pub fn a_mod_message_goes_from_one_package_to_another_sf180() {
    mod_message_loopback(SourceVersion::Sf180);
}

// ---- the production family through the worker ----

static VERSIONS: LazyLock<Mutex<HashMap<KilnOwner, SourceVersion>>> =
    LazyLock::new(Default::default);
static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}
const SOURCE: &str = r#"
local names = {}
for k, v in pairs(runtime.network) do names[#names + 1] = k .. ':' .. type(v) end
table.sort(names)
record('top', table.concat(names, ','), tostring(runtime.network.status().reason))
return {
  on_load = function()
    local status = runtime.network.status()
    local _, send = runtime.network.send('76561198000000042', 'x')
    local _, receive = runtime.network.receive()
    local _, accept = runtime.network.accept('76561198000000042')
    local _, send_mod = runtime.network.send_mod('76561198000000042', 'b', 'p')
    local _, receive_mod = runtime.network.receive_mod()
    record('load', tostring(status.available), status.role, tostring(status.reason), send, receive, accept,
      send_mod, receive_mod)
  end,
}
"#;
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
const IDS: [&str; 2] = ["a", "b"];
struct NetworkPlan;
impl PlanSource for NetworkPlan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let owner = cx.owner();
        let version = VERSIONS.lock().unwrap()[&owner];
        let mut registry = Registry::default();
        for id in IDS {
            let source: Rc<dyn PackageSource> = Rc::new(Source);
            let mut parsed = package::Package::parse(
                json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]})
                    .to_string()
                    .as_bytes(),
                None,
                package::ParseOptions::default(),
            )
            .unwrap();
            parsed.manifest.enabled = true;
            registry
                .insert(
                    PackageRecord::new(
                        id.into(),
                        parsed,
                        RuntimePackage::new(id.to_owned(), source.clone()),
                        source,
                    )
                    .unwrap(),
                )
                .unwrap();
        }
        let register: Register = Box::new(move |lua, env, package, _| {
            let id = package.id().to_owned();
            env.set(
                "record",
                lua.create_function(move |_, parts: mlua::Variadic<String>| {
                    let line = format!("{id}:{}", parts.join("|"));
                    RECORDS.lock().unwrap().entry(owner).or_default().push(line);
                    Ok(())
                })?,
            )
        });
        Ok(RuntimePlan {
            side: Side::Client,
            version,
            registry,
            plan: IDS.map(String::from).to_vec(),
            admitted: IDS.map(String::from).into(),
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
    fn families(_: &WorkerContext<'_>) -> Result<Families, String> {
        Families::new(vec![network::production_family()])
    }
    fn game_build(_: &WorkerContext<'_>) -> String {
        BUILD.to_owned()
    }
}

fn through_the_worker(version: SourceVersion) {
    let owner = next_owner();
    VERSIONS.lock().unwrap().insert(owner, version);
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<NetworkPlan>)));
    let api: &'static KilnApi = Box::leak(api(true));
    // SAFETY: the fake table lives for the process.
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    let done = |id: &str| {
        records(owner)
            .iter()
            .any(|r| r.starts_with(&format!("{id}:load")))
    };
    assert!(
        eventually(WAIT, || done("a") && done("b")),
        "{:?} {:?}",
        records(owner),
        shared.status()
    );
    shared.request_stop();
    assert!(pool.task(0).wait_finished(WAIT));
    let seen = records(owner);
    let members = "accept:function,connected_peers:function,receive:function,receive_mod:function,send:function,send_mod:function,status:function";
    for id in IDS {
        let expected = [
            format!("{id}:top|{members}|{NOT_ACTIVE}"),
            format!(
                "{id}:load|false|client|{}|{}|{}|{}|{}|{}",
                unverified("runtime.network.status"),
                unverified("runtime.network.send"),
                unverified("runtime.network.receive"),
                unverified("runtime.network.accept"),
                // send_mod and receive_mod are gated as send and receive (ShroudForge).
                unverified("runtime.network.send"),
                unverified("runtime.network.receive"),
            ),
        ];
        for line in expected {
            assert!(seen.contains(&line), "{line} in {seen:?}");
        }
    }
}

/// The production family through the worker on ShroudForge 1.7.9's catalog.
pub fn the_production_family_installs_through_the_worker_sf179() {
    through_the_worker(SourceVersion::Sf179);
}
/// The same on 1.8.0's.
pub fn the_production_family_installs_through_the_worker_sf180() {
    through_the_worker(SourceVersion::Sf180);
}
