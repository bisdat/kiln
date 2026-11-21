// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family tests.
//! The runtime.network family core (status, send, receive, accept) on the real service
//! adapter, the registrar's production host (KilnRuntimeHost) and the C-ABI fake kiln.net with the
//! internal Steam-messages block, in a real Lua VM; plus the production family
//! through the real worker and lifecycle host. The goldens of both pins are in
//! runtime_network_proof.rs.
//! Expected texts and orders are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), both pins:
//! modules/transport/lua/network.rs:17-348, modules/transport/lib.rs:7-311,
//! modules/mod-runtime/lua/ingame/check_ingame_access.rs:61-72,227-237,386-414.
#[path = "support/fake_services/mod.rs"]
mod fake_services;
mod support;

use fake_services::{Inbound, Shape};
use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_net::KilnNetApi;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::availability::{FactsConfig, KilnRuntimeHost, OwnerActivity, RuntimeFacts};
use kiln_sfi::runtime::network::l16::*;
use kiln_sfi::runtime::network::{
    self, CAPABILITY, CLAIMS, NetworkPorts, NetworkShared, install_into, text,
};
use kiln_sfi::runtime::operation_catalog::CATALOG;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::{Families, Family};
use kiln_sfi::worker::compose::ComposePlan;
use kiln_sfi::worker::*;
use mlua::{FromLuaMulti, Lua, Table, Value};
use serde_json::json;
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, register_runtime};
use sfi_core::compose::GameTypes;
use sfi_core::host::{
    HostContext, Package as RuntimePackage, PackageSource, Phase, Side, SourceVersion,
};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
use sfi_core::lua::LuaVm;
use sfi_core::package::{
    self, Manifest,
    discover::{PackageRecord, Registry},
};
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;
use std::sync::{LazyLock, Mutex, MutexGuard};
use std::time::Duration;
use support::*;

const BUILD: &str = "test-build";
const PEER: u64 = 76_561_198_000_000_042;
const PEER_TEXT: &str = "76561198000000042";
const NOT_ACTIVE: &str =
    "runtime API access is available only while this mod's lifecycle is active";
const PREGAME: &str = "runtime APIs are available only during the ingame phase";
const PROVIDER: &str =
    "Steam Networking Messages transport or session callback is unavailable in this process";
/// A liveness bound for what must happen; a passing wait returns as soon as it holds.
const WAIT: Duration = Duration::from_secs(300);

fn unverified(operation: &str) -> String {
    format!("operation {operation} has not been verified for game build {BUILD}")
}

/// kiln.caps rows are process-wide; the tests of this binary take turns.
static SERIAL: Mutex<()> = Mutex::new(());
fn serial() -> MutexGuard<'static, ()> {
    let guard = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    fake_services::set_cap(CAPABILITY, Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)));
    guard
}

/// The maintenance's ports, scripted.
#[derive(Default)]
struct Ports {
    authorized: RefCell<HashSet<u64>>,
    local: Cell<Option<u64>>,
    remote: Cell<Option<u64>>,
}
impl NetworkPorts for Ports {
    fn authorized_server_peer(&self, peer: u64) -> bool {
        self.authorized.borrow().contains(&peer)
    }
    fn local_dedicated_server_steam_id(&self) -> Option<u64> {
        self.local.get()
    }
    fn remote_dedicated_server_steam_id(&self) -> Option<u64> {
        self.remote.get()
    }
}

struct Active(Cell<bool>);
impl OwnerActivity for Active {
    fn active(&self) -> bool {
        self.0.get()
    }
}
fn manifest(id: &str, capabilities: &[&str]) -> Manifest {
    package::Package::parse(
        json!({"id": id, "name": id, "version": "1.0.0", "capabilities": capabilities})
            .to_string()
            .as_bytes(),
        None,
        package::ParseOptions::default(),
    )
    .unwrap()
    .manifest
}
struct NoFiles;
impl PackageSource for NoFiles {
    fn read_file(&self, _: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::ErrorKind::NotFound.into())
    }
}

/// One package with the network family on the registrar's production host, on this thread.
struct Net {
    owner: KilnOwner,
    vm: LuaVm,
    env: Table,
    active: Rc<Active>,
    ports: Rc<Ports>,
    shared: Rc<NetworkShared>,
    services: Rc<ServiceAdapter>,
}
#[derive(Clone, Copy)]
struct Setup {
    id: &'static str,
    capabilities: &'static [&'static str],
    phase: Phase,
    side: Side,
    active: bool,
    version: SourceVersion,
}
const SERVER: Setup = Setup {
    id: "alpha",
    capabilities: &["runtime"],
    phase: Phase::Runtime,
    side: Side::Server,
    active: true,
    version: SourceVersion::Sf179,
};
const CLIENT: Setup = Setup {
    side: Side::Client,
    ..SERVER
};

fn net(setup: Setup) -> Net {
    let owner = next_owner();
    fake_services::bind_thread(owner);
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
    let context = HostContext::new(setup.phase, setup.side, setup.version);
    let active = Rc::new(Active(Cell::new(setup.active)));
    let host: Rc<dyn RuntimeHost> = Rc::new(KilnRuntimeHost::new(
        &manifest(setup.id, setup.capabilities),
        &context,
        active.clone(),
        facts,
    ));
    let ports = Rc::new(Ports::default());
    let shared = Rc::new(NetworkShared::new(ports.clone()));
    let vm = LuaVm::new(context).unwrap();
    let env = {
        let (host, services, shared) = (host.clone(), services.clone(), shared.clone());
        vm.add_package(
            RuntimePackage::new(setup.id, Rc::new(NoFiles)),
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
    Net {
        owner,
        vm,
        env,
        active,
        ports,
        shared,
        services,
    }
}
impl Net {
    fn eval<T: FromLuaMulti>(&self, source: &str) -> mlua::Result<T> {
        self.vm
            .lua()
            .load(source)
            .set_name("=runtime_network")
            .set_environment(self.env.clone())
            .eval()
    }
    /// `(ok, reason)` of a call that returns them.
    fn pair(&self, call: &str) -> (Value, Option<String>) {
        self.eval(&format!("return {call}")).unwrap()
    }
    fn ok_reason(&self, call: &str) -> (bool, Option<String>) {
        self.eval(&format!("return {call}")).unwrap()
    }
    fn raises(&self, call: &str) -> String {
        root(
            &self
                .eval::<mlua::MultiValue>(&format!("return {call}"))
                .expect_err(call),
        )
    }
    fn calls(&self, member: &str) -> u64 {
        fake_services::counters(self.owner).calls(member)
    }
    fn net<R>(&self, f: impl FnOnce(&mut fake_services::NetFake) -> R) -> R {
        fake_services::net(self.owner, f)
    }
    fn sent(&self) -> Vec<fake_services::Sent> {
        self.net(|n| n.sent.clone())
    }
    fn network_calls(&self) -> u64 {
        [
            "net.open",
            "net.send",
            "net.receive",
            "net.accept",
            "net.close",
            "net.maintain",
        ]
        .iter()
        .map(|m| self.calls(m))
        .sum()
    }
    /// The status table as sorted `key=value` text.
    fn status(&self) -> String {
        self.eval(
            "local s = runtime.network.status(); local keys = {}
             for k, v in pairs(s) do
               if type(v) == 'table' then
                 local items = {}
                 for i, x in ipairs(v) do items[i] = x end
                 v = '[' .. table.concat(items, ',') .. ']'
               end
               keys[#keys + 1] = k .. '=' .. tostring(v)
             end
             table.sort(keys); return table.concat(keys, ';')",
        )
        .unwrap()
    }
}
fn root(error: &mlua::Error) -> String {
    match error {
        mlua::Error::CallbackError { cause, .. } => root(cause),
        other => other.to_string(),
    }
}

// ---- typed arguments, then the denial chain ----

#[test]
fn typed_arguments_raise_before_the_denial_even_for_a_denied_mod() {
    let _serial = serial();
    for (setup, denial) in [
        (
            Setup {
                phase: Phase::Startup,
                ..SERVER
            },
            PREGAME.to_owned(),
        ),
        (
            Setup {
                capabilities: &[],
                ..SERVER
            },
            "mod 'alpha' requires capabilities: [\"runtime\"]".to_owned(),
        ),
        (
            Setup {
                active: false,
                ..SERVER
            },
            NOT_ACTIVE.to_owned(),
        ),
    ] {
        let n = net(setup);
        for (call, message) in [
            (
                "runtime.network.send()",
                "bad argument #1: error converting Lua nil to String",
            ),
            (
                "runtime.network.send({}, 'x')",
                "bad argument #1: error converting Lua table to String",
            ),
            (
                "runtime.network.send('\\255', 'x')",
                "bad argument #1: error converting Lua string to String (invalid utf-8",
            ),
            (
                "runtime.network.send('1')",
                "bad argument #2: error converting Lua nil to string",
            ),
            (
                "runtime.network.send('1', true)",
                "bad argument #2: error converting Lua boolean to string",
            ),
            (
                "runtime.network.send('1', 'x', 5)",
                "bad argument #3: error converting Lua integer to table",
            ),
            (
                "runtime.network.receive('x')",
                "bad argument #1: error converting Lua string to table",
            ),
            (
                "runtime.network.accept()",
                "bad argument #1: error converting Lua nil to String",
            ),
            (
                "runtime.network.accept({})",
                "bad argument #1: error converting Lua table to String",
            ),
        ] {
            let error = n.raises(call);
            assert!(error.starts_with(message), "{call}: {error}");
        }
        // Valid types reach the denial; options are read only after it.
        for call in [
            "runtime.network.send(7, 5)",
            "runtime.network.send('0', '', {channel = 70000})",
            "runtime.network.accept('0')",
        ] {
            assert_eq!(n.ok_reason(call), (false, Some(denial.clone())), "{call}");
        }
        assert_eq!(
            n.pair("runtime.network.receive({channel = -1, limit = 'x'})"),
            (Value::Nil, Some(denial.clone()))
        );
        assert_eq!(
            n.network_calls(),
            0,
            "a denied call reaches no lease member"
        );
    }
}

#[test]
fn every_gate_answers_in_shroudforge_s_order() {
    let _serial = serial();
    // (what is wrong, the reason) in the chain's order; each case also breaks every later gate,
    // so only the order can pick the reason.
    #[derive(Clone, Copy, Debug)]
    enum Broken {
        Pregame,
        Permission,
        Lifecycle,
        Static,
        Settings,
        Disabled,
        NotAvailable,
        NotRegistered,
        Nothing,
    }
    use Broken::*;
    for op in ["status", "send", "receive", "accept"] {
        for (index, broken) in [
            Pregame,
            Permission,
            Lifecycle,
            Static,
            Settings,
            Disabled,
            NotAvailable,
            NotRegistered,
            Nothing,
        ]
        .into_iter()
        .enumerate()
        {
            let at = |gate: usize| index <= gate;
            let id: &'static str = Box::leak(format!("gate-{op}-{index}").into_boxed_str());
            let n = net(Setup {
                id,
                phase: if at(0) {
                    Phase::Startup
                } else {
                    Phase::Runtime
                },
                capabilities: if at(1) { &[] } else { &["runtime"] },
                active: !at(2),
                ..SERVER
            });
            fake_services::set_cap(
                CAPABILITY,
                (!at(3)).then_some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)),
            );
            if at(4) {
                n.shared.settings().replace(Err("unreadable".into()));
            } else if at(5) {
                n.shared
                    .settings()
                    .replace(Ok(json!({"modules": {"network": {"enabled": false}}})));
            }
            n.net(|f| {
                f.flags = 0;
                if !at(6) {
                    f.flags |= KILN_STEAM_MSG_AVAILABLE;
                }
                if !at(7) {
                    f.flags |= KILN_STEAM_MSG_REGISTERED;
                }
            });
            let operation = format!("runtime.network.{op}");
            let reason = match broken {
                Pregame => Some(PREGAME.to_owned()),
                Permission => Some(format!("mod '{id}' requires capabilities: [\"runtime\"]")),
                Lifecycle => Some(NOT_ACTIVE.to_owned()),
                Static => Some(unverified(&operation)),
                Settings => Some(text::SETTINGS_UNAVAILABLE.to_owned()),
                Disabled => Some(text::DISABLED.to_owned()),
                NotAvailable => Some(text::NOT_INITIALIZED.to_owned()),
                NotRegistered => Some(text::NOT_REGISTERED.to_owned()),
                Nothing => None,
            };
            let case = format!("{op} {broken:?}");
            let got = match op {
                "status" => n
                    .eval::<Option<String>>("return runtime.network.status().reason")
                    .unwrap(),
                "send" => {
                    n.ok_reason(&format!("runtime.network.send('{PEER_TEXT}', 'x')"))
                        .1
                }
                "receive" => n.pair("runtime.network.receive()").1,
                _ => {
                    n.ok_reason(&format!("runtime.network.accept('{PEER_TEXT}')"))
                        .1
                }
            };
            // An admitted accept on a server still needs an authorized peer (the maintenance's set).
            let expected = match (op, broken) {
                ("accept", Nothing) => Some(text::NOT_AUTHORIZED.to_owned()),
                _ => reason,
            };
            assert_eq!(got, expected, "{case}");
            fake_services::set_cap(CAPABILITY, Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)));
        }
    }
}

#[test]
fn the_disabled_warning_is_logged_once_per_mod_id() {
    let _serial = serial();
    let disabled = || Ok(json!({"modules": {"network": {"enabled": false}}}));
    let a = net(Setup {
        id: "warn-once-a",
        ..SERVER
    });
    a.shared.settings().replace(disabled());
    for call in [
        "runtime.network.status()",
        "runtime.network.send('1', 'x')",
        "runtime.network.receive()",
        "runtime.network.accept('1')",
        "runtime.network.send('1', 'x')",
    ] {
        a.eval::<mlua::MultiValue>(&format!("return {call}"))
            .unwrap();
    }
    let warnings = |owner| {
        logs(owner)
            .into_iter()
            .filter(|(level, line)| {
                *level == KILN_LOG_WARN && line.contains(text::DISABLED_WARNING)
            })
            .collect::<Vec<_>>()
    };
    assert_eq!(
        warnings(a.owner),
        [(
            KILN_LOG_WARN,
            format!(
                "shroudforge::network: {} (mod_id=warn-once-a, operation=runtime.network.status)",
                text::DISABLED_WARNING
            )
        )]
    );
    // Once per mod id for the process: another runtime-create of the same package says nothing
    // more; another package is warned once.
    let again = net(Setup {
        id: "warn-once-a",
        ..SERVER
    });
    again.shared.settings().replace(disabled());
    let _ = again.ok_reason("runtime.network.send('1', 'x')");
    assert!(warnings(again.owner).is_empty());
    let b = net(Setup {
        id: "warn-once-b",
        ..SERVER
    });
    b.shared.settings().replace(disabled());
    let _ = b.ok_reason("runtime.network.send('1', 'x')");
    let _ = b.ok_reason("runtime.network.send('1', 'x')");
    assert_eq!(warnings(b.owner).len(), 1);
    // Only an explicit false disables: a missing block, null or "false" does not.
    for document in [
        json!({}),
        json!({"modules": {"network": {"enabled": null}}}),
        json!({"modules": {"network": {"enabled": "false"}}}),
        json!({"modules": {"network": {"enabled": 0}}}),
    ] {
        b.shared.settings().replace(Ok(document.clone()));
        assert_eq!(
            b.ok_reason(&format!("runtime.network.send('{PEER_TEXT}', 'x')")),
            (true, None),
            "{document}"
        );
    }
}

// ---- send ----

#[test]
fn send_reads_peer_channel_and_reliable_as_shroudforge_does() {
    let _serial = serial();
    let n = net(SERVER);
    for (peer, reason) in [
        ("0", options_text::PEER_ZERO),
        ("00", options_text::PEER_ZERO),
        ("-1", options_text::PEER_NOT_DECIMAL),
        ("1.5", options_text::PEER_NOT_DECIMAL),
        ("18446744073709551616", options_text::PEER_NOT_DECIMAL),
        (" 1", options_text::PEER_NOT_DECIMAL),
    ] {
        assert_eq!(
            n.ok_reason(&format!("runtime.network.send('{peer}', 'x')")),
            (false, Some(reason.to_owned())),
            "{peer}"
        );
    }
    for channel in ["65535", "-1", "70000", "-1.5", "65535.5"] {
        assert_eq!(
            n.raises(&format!(
                "runtime.network.send('1', 'x', {{channel = {channel}}})"
            )),
            format!("runtime error: {}", options_text::CHANNEL_RANGE),
            "{channel}"
        );
    }
    assert!(
        n.raises("runtime.network.send('1', 'x', {channel = 'x'})")
            .starts_with("error converting Lua string to i32"),
    );
    assert_eq!(n.sent(), [], "nothing sent for a refused argument");
    // Truncation, leading zeros, the default channel and Lua truthiness of `reliable`.
    for (options, channel, flags) in [
        ("nil", 0, 40),
        ("{}", 0, 40),
        ("{channel = -0.5}", 0, 40),
        ("{channel = 1.9}", 1, 40),
        ("{channel = 65534.9}", 65534, 40),
        ("{reliable = false}", 0, 32),
        ("{reliable = 0}", 0, 40),
        ("{reliable = ''}", 0, 40),
        ("{reliable = {}}", 0, 40),
        ("{reliable = nil}", 0, 40),
    ] {
        n.net(|f| f.sent.clear());
        assert_eq!(
            n.ok_reason(&format!(
                "runtime.network.send('000{PEER_TEXT}', 'x', {options})"
            )),
            (true, None),
            "{options}"
        );
        let sent = n.sent();
        assert_eq!(sent.len(), 1, "{options}");
        assert_eq!(
            (sent[0].peer, sent[0].channel, sent[0].flags),
            (PEER, channel, flags),
            "{options}"
        );
    }
    // A number peer is converted by Lua first (ShroudForge's String argument).
    n.net(|f| f.sent.clear());
    assert_eq!(
        n.ok_reason("runtime.network.send(76561198000000042, 'x')"),
        (true, None)
    );
    assert_eq!(n.sent()[0].peer, PEER);
}

/// The fixture texts of options.rs.
mod options_text {
    pub use kiln_sfi::runtime::network::options::*;
}

#[test]
fn send_moves_any_bytes_up_to_512_kib_and_maps_each_result() {
    let _serial = serial();
    let n = net(SERVER);
    for (payload, bytes) in [
        ("''", Vec::new()),
        ("'\\0'", vec![0u8]),
        ("'\\255\\254a\\0'", vec![255, 254, b'a', 0]),
        ("string.rep('z', 524288)", vec![b'z'; 524_288]),
    ] {
        n.net(|f| f.sent.clear());
        assert_eq!(
            n.ok_reason(&format!("runtime.network.send('{PEER_TEXT}', {payload})")),
            (true, None),
            "{payload}"
        );
        assert_eq!(n.sent()[0].data, bytes, "{payload}");
    }
    let before = n.calls("net.send");
    assert_eq!(
        n.ok_reason(&format!(
            "runtime.network.send('{PEER_TEXT}', string.rep('z', 524289))"
        )),
        (false, Some("payload exceeds 524288 bytes".to_owned()))
    );
    assert_eq!(n.calls("net.send"), before, "too big: no lease call");
    for (send_result, eresult, expected) in [
        (KILN_OK, 1, (true, None)),
        (
            KILN_OK,
            3,
            (false, Some("Steam returned EResult 3".to_owned())),
        ),
        (
            KILN_OK,
            0,
            (false, Some("Steam returned EResult 0".to_owned())),
        ),
        (
            KILN_OK,
            25,
            (false, Some("Steam returned EResult 25".to_owned())),
        ),
        (
            KILN_ERR_UNAVAILABLE,
            1,
            (false, Some(text::INTERFACE_UNAVAILABLE.to_owned())),
        ),
        (
            KILN_ERR_INTERNAL,
            1,
            (false, Some(text::INTERFACE_UNAVAILABLE.to_owned())),
        ),
        (
            KILN_ERR_INVALID_ARGUMENT,
            1,
            (false, Some(text::INTERFACE_UNAVAILABLE.to_owned())),
        ),
    ] {
        n.net(|f| {
            f.send_result = send_result;
            f.eresult = eresult;
        });
        assert_eq!(
            n.ok_reason(&format!("runtime.network.send('{PEER_TEXT}', 'x')")),
            expected,
            "{send_result} {eresult}"
        );
    }
}

// ---- a missing member against Steam not ready ----

#[test]
fn a_missing_member_and_steam_not_ready_answer_different_texts() {
    let _serial = serial();
    let n = net(SERVER);
    let slot = |member: usize| Some(Shape::NullAt(member));
    for (shape, send, receive, accept) in [
        (
            slot(std::mem::offset_of!(KilnNetApi, steam_send)),
            Some(text::MODULE_UNAVAILABLE),
            None,
            Some(text::NOT_AUTHORIZED),
        ),
        (
            slot(std::mem::offset_of!(KilnNetApi, steam_receive)),
            None,
            Some(text::MODULE_UNAVAILABLE),
            Some(text::NOT_AUTHORIZED),
        ),
        (
            slot(std::mem::offset_of!(KilnNetApi, steam_accept)),
            None,
            None,
            Some(text::MODULE_UNAVAILABLE),
        ),
        (
            slot(std::mem::offset_of!(KilnNetApi, steam_open)),
            Some(text::MODULE_UNAVAILABLE),
            Some(text::MODULE_UNAVAILABLE),
            Some(text::MODULE_UNAVAILABLE),
        ),
        // The table ends right before receive's slot: never read past its size.
        (
            Some(Shape::Short(
                std::mem::offset_of!(KilnNetApi, steam_receive) as u32,
            )),
            None,
            Some(text::MODULE_UNAVAILABLE),
            Some(text::MODULE_UNAVAILABLE),
        ),
    ] {
        n.ports.authorized.borrow_mut().clear();
        if accept != Some(text::NOT_AUTHORIZED) {
            n.ports.authorized.borrow_mut().insert(PEER);
        }
        // A fresh lease each round, so a missing `open` is seen.
        n.shared.take_lease();
        n.net(|f| f.leases.clear());
        fake_services::set_shape("kiln.net", shape);
        let got_send = n
            .ok_reason(&format!("runtime.network.send('{PEER_TEXT}', 'x')"))
            .1;
        let got_receive = n.pair("runtime.network.receive()").1;
        let got_accept = n
            .ok_reason(&format!("runtime.network.accept('{PEER_TEXT}')"))
            .1;
        fake_services::set_shape("kiln.net", None);
        assert_eq!(
            (
                got_send.as_deref(),
                got_receive.as_deref(),
                got_accept.as_deref()
            ),
            (send, receive, accept),
            "{shape:?}"
        );
    }
    // Without the lease's status member (a 128-byte kiln.net v1), the operations are outside
    // this build's support: the catalog's reason, before any other gate.
    for shape in [
        Shape::Short(128),
        Shape::NullAt(std::mem::offset_of!(KilnNetApi, steam_status)),
        Shape::Missing,
    ] {
        fake_services::set_shape("kiln.net", Some(shape));
        let got = n.ok_reason(&format!("runtime.network.send('{PEER_TEXT}', 'x')"));
        let status = n.status();
        fake_services::set_shape("kiln.net", None);
        assert_eq!(
            got,
            (false, Some(unverified("runtime.network.send"))),
            "{shape:?}"
        );
        assert!(
            status.contains(&format!("reason={}", unverified("runtime.network.status"))),
            "{status}"
        );
    }
    // Steam not ready (AVAILABLE clear), or status refusing (closed for exit): not initialized.
    for (flags, result) in [
        (KILN_STEAM_MSG_REGISTERED, KILN_OK),
        (
            KILN_STEAM_MSG_AVAILABLE | KILN_STEAM_MSG_REGISTERED,
            KILN_ERR_UNAVAILABLE,
        ),
    ] {
        n.net(|f| {
            f.flags = flags;
            f.status_result = result;
        });
        assert_eq!(
            n.ok_reason(&format!("runtime.network.send('{PEER_TEXT}', 'x')")),
            (false, Some(text::NOT_INITIALIZED.to_owned()))
        );
    }
}

// ---- receive ----

fn message(peer: u64, data: &[u8], reliable: bool) -> Inbound {
    Inbound::Message {
        peer,
        data: data.to_vec(),
        reliable,
    }
}
/// The messages of a receive call as `peer|payload-bytes|reliable` lines, and its reason.
fn received(n: &Net, call: &str) -> (Option<Vec<String>>, Option<String>) {
    let (messages, reason): (Option<Table>, Option<String>) =
        n.eval(&format!("return {call}")).unwrap();
    let messages = messages.map(|t| {
        t.sequence_values::<Table>()
            .map(|m| {
                let m = m.unwrap();
                let payload: mlua::String = m.raw_get("payload").unwrap();
                format!(
                    "{}|{:?}|{}",
                    m.raw_get::<String>("peer_steam_id").unwrap(),
                    payload.as_bytes().to_vec(),
                    m.raw_get::<bool>("reliable").unwrap()
                )
            })
            .collect()
    });
    (messages, reason)
}

#[test]
fn receive_takes_at_most_limit_messages_and_dropped_ones_count() {
    let _serial = serial();
    let n = net(SERVER);
    let other = PEER + 1;
    n.ports.authorized.borrow_mut().insert(PEER);
    queue_many(
        &n,
        3,
        &[
            message(PEER, b"", true),
            message(other, b"dropped", true),
            message(PEER, b"\0\xff", false),
            message(PEER, b"third", true),
        ],
    );
    // limit 3: three attempts; the unauthorized sender's message is dropped and still counts.
    assert_eq!(
        received(&n, "runtime.network.receive({channel = 3, limit = 3})"),
        (
            Some(vec![
                format!("{PEER}|[]|true"),
                format!("{PEER}|[0, 255]|false"),
            ]),
            None
        )
    );
    assert_eq!(n.calls("net.receive"), 3);
    // The rest, then an empty queue ends the loop with one more call.
    assert_eq!(
        received(&n, "runtime.network.receive({channel = 3})"),
        (
            Some(vec![format!("{PEER}|{:?}|true", b"third".to_vec())]),
            None
        )
    );
    assert_eq!(n.calls("net.receive"), 5);
    // A client keeps every sender (no authorization), on the default channel 0.
    let c = net(CLIENT);
    queue_many(
        &c,
        0,
        &[message(other, b"a", true), message(PEER, b"b", false)],
    );
    assert_eq!(
        received(&c, "runtime.network.receive()"),
        (
            Some(vec![
                format!("{other}|[97]|true"),
                format!("{PEER}|[98]|false"),
            ]),
            None
        )
    );
}
fn queue_many(n: &Net, channel: u32, steps: &[Inbound]) {
    fake_services::queue(n.owner, channel, steps.iter().cloned());
}

#[test]
fn a_failure_mid_loop_returns_what_was_taken_and_the_reason() {
    let _serial = serial();
    let n = net(CLIENT);
    for (failure, reason) in [
        (Inbound::Result(KILN_ERR_INTERNAL), text::RECEIVE_FAILED),
        (Inbound::Result(KILN_ERR_UNAVAILABLE), text::RECEIVE_FAILED),
        (
            Inbound::Result(KILN_STEAM_ERR_INVALID_DATA),
            text::RECEIVE_FAILED,
        ),
        // Too big for the buffer or no Steam id: the lease drops it and answers INVALID_DATA.
        (
            message(PEER, &vec![1u8; 524_289], true),
            text::RECEIVE_FAILED,
        ),
        (message(0, b"x", true), text::RECEIVE_FAILED),
    ] {
        queue_many(
            &n,
            0,
            &[
                message(PEER, b"a", true),
                failure.clone(),
                message(PEER, b"b", true),
            ],
        );
        assert_eq!(
            received(&n, "runtime.network.receive()"),
            (
                Some(vec![format!("{PEER}|[97]|true")]),
                Some(reason.to_owned())
            ),
            "{:?}",
            match &failure {
                Inbound::Result(code) => format!("code {code}"),
                Inbound::Message { peer, data, .. } => format!("message {peer} {}", data.len()),
            }
        );
        n.net(|f| f.inbound.clear());
    }
    // A full 512 KiB message fits.
    queue_many(&n, 0, &[message(PEER, &vec![7u8; 524_288], true)]);
    let (messages, reason) = received(&n, "runtime.network.receive({limit = 1})");
    assert_eq!((messages.map(|m| m.len()), reason), (Some(1), None));
    // A lease that cannot be opened: the receive text, and an empty table.
    let m = net(CLIENT);
    m.net(|f| f.open_result = KILN_ERR_UNAVAILABLE);
    assert_eq!(
        received(&m, "runtime.network.receive()"),
        (Some(vec![]), Some(text::RECEIVE_FAILED.to_owned()))
    );
}

#[test]
fn receive_options_raise_shroudforge_s_texts_after_the_denial() {
    let _serial = serial();
    let n = net(CLIENT);
    for options in [
        "{limit = 0}",
        "{limit = 33}",
        "{limit = -0.5}",
        "{limit = 0.9}",
    ] {
        assert_eq!(
            n.raises(&format!("runtime.network.receive({options})")),
            format!("runtime error: {}", options_text::LIMIT_RANGE),
            "{options}"
        );
    }
    // A negative integer is a conversion error of the usize option, as in ShroudForge.
    assert_eq!(
        n.raises("runtime.network.receive({limit = -1})"),
        "error converting Lua integer to usize (out of range)"
    );
    assert_eq!(
        n.raises("runtime.network.receive({channel = 65535, limit = 0})"),
        format!("runtime error: {}", options_text::CHANNEL_RANGE)
    );
    for (options, attempts) in [
        ("{limit = 1.9}", 1),
        ("{limit = 32.9}", 32),
        ("{}", 16),
        ("nil", 16),
    ] {
        let before = n.calls("net.receive");
        queue_many(&n, 0, &vec![message(PEER, b"m", true); 40]);
        let (messages, _) = received(&n, &format!("runtime.network.receive({options})"));
        assert_eq!(messages.unwrap().len(), attempts, "{options}");
        assert_eq!(
            n.calls("net.receive") - before,
            attempts as u64,
            "{options}"
        );
        n.net(|f| f.inbound.clear());
    }
}

// ---- accept ----

#[test]
fn accept_checks_authorization_on_a_server_and_maps_each_result() {
    let _serial = serial();
    let s = net(SERVER);
    assert_eq!(
        s.ok_reason(&format!("runtime.network.accept('{PEER_TEXT}')")),
        (false, Some(text::NOT_AUTHORIZED.to_owned()))
    );
    assert_eq!(s.calls("net.accept"), 0);
    s.ports.authorized.borrow_mut().insert(PEER);
    for (result, answer, expected) in [
        (KILN_OK, 1, (true, None)),
        (
            KILN_OK,
            0,
            (false, Some(text::NO_PENDING_SESSION.to_owned())),
        ),
        (
            KILN_ERR_UNAVAILABLE,
            1,
            (false, Some(text::INTERFACE_UNAVAILABLE.to_owned())),
        ),
        (
            KILN_ERR_INTERNAL,
            1,
            (false, Some(text::INTERFACE_UNAVAILABLE.to_owned())),
        ),
    ] {
        s.net(|f| {
            f.accept_result = result;
            f.accepted_answer = answer;
        });
        assert_eq!(
            s.ok_reason(&format!("runtime.network.accept('{PEER_TEXT}')")),
            expected
        );
    }
    assert_eq!(s.net(|f| f.accepted.clone()), [PEER, PEER]);
    // A client accepts any peer it is asked to.
    let c = net(CLIENT);
    assert_eq!(c.ok_reason("runtime.network.accept('0001')"), (true, None));
    assert_eq!(c.net(|f| f.accepted.clone()), [1]);
    assert_eq!(
        c.ok_reason("runtime.network.accept('x')"),
        (false, Some(options_text::PEER_NOT_DECIMAL.to_owned()))
    );
}

// ---- the lease ----

#[test]
fn one_lease_is_opened_on_first_use_and_a_refused_open_is_tried_again() {
    let _serial = serial();
    let n = net(CLIENT);
    let _ = n.status();
    assert_eq!(n.calls("net.open"), 0, "status needs no lease");
    n.net(|f| f.open_result = KILN_ERR_UNAVAILABLE);
    assert_eq!(
        n.ok_reason(&format!("runtime.network.send('{PEER_TEXT}', 'x')")),
        (false, Some(text::INTERFACE_UNAVAILABLE.to_owned()))
    );
    assert_eq!(
        n.ok_reason(&format!("runtime.network.accept('{PEER_TEXT}')")),
        (false, Some(text::INTERFACE_UNAVAILABLE.to_owned()))
    );
    assert_eq!(n.shared.lease(), None);
    n.net(|f| f.open_result = KILN_OK);
    for _ in 0..3 {
        assert_eq!(
            n.ok_reason(&format!("runtime.network.send('{PEER_TEXT}', 'x')")),
            (true, None)
        );
    }
    let _ = received(&n, "runtime.network.receive()");
    let _ = n.ok_reason(&format!("runtime.network.accept('{PEER_TEXT}')"));
    assert_eq!(n.calls("net.open"), 3, "two refused, one opened");
    assert_eq!(
        n.net(|f| f.leases.clone()),
        [(n.owner, 0, 65_535, KILN_STEAM_OPEN_SESSIONS, true)]
    );
    let lease = n.shared.lease().unwrap();
    assert!(n.sent().iter().all(|s| s.lease == lease));
    // The lease is handed over once (the maintenance closes it).
    assert_eq!(n.shared.take_lease(), Some(lease));
    assert_eq!(n.shared.take_lease(), None);
    let _ = n.services.counters();
}

// ---- status ----

#[test]
fn status_fills_or_omits_each_key_as_shroudforge_does() {
    let _serial = serial();
    let s = net(SERVER);
    // Admitted, identity known; the discovery ports are read only while available.
    s.ports.local.set(Some(11));
    s.ports.remote.set(Some(0));
    assert_eq!(
        s.status(),
        "allowed_client_steam_ids=[];available=true;local_dedicated_server_steam_id=11;\
         local_steam_id=76561198000000001;role=server;service_ready=true"
    );
    // Settings: a valid configured id, the allowlist parsed (duplicates and leading zeros kept,
    // bad entries dropped).
    s.shared.settings().replace(Ok(json!({"modules": {"network": {
        "enabled": true,
        "serverSteamId": "0076561198000000099",
        "allowedClientSteamIds": " 76561198000000001;76561198000000001 ,0076561198000000002,1,abc,76561198000000003x,00000000000000000,;"
    }}})));
    s.net(|f| f.flags = KILN_STEAM_MSG_AVAILABLE | KILN_STEAM_MSG_REGISTERED);
    s.ports.remote.set(Some(22));
    assert_eq!(
        s.status(),
        "allowed_client_steam_ids=[76561198000000001,76561198000000001,0076561198000000002];available=true;\
         configured_server_steam_id=0076561198000000099;local_dedicated_server_steam_id=11;\
         remote_dedicated_server_steam_id=22;role=server;service_ready=true"
    );
    for server in ["0", "", "x", "18446744073709551616"] {
        s.shared.settings().replace(Ok(
            json!({"modules": {"network": {"serverSteamId": server}}}),
        ));
        assert!(
            !s.status().contains("configured_server_steam_id"),
            "{server}"
        );
    }
    // Not available: no native field, the ports unread, "not initialized".
    s.shared
        .settings()
        .replace(Ok(network::settings::default_document()));
    s.net(|f| f.flags = KILN_STEAM_MSG_REGISTERED | KILN_STEAM_MSG_IDENTITY);
    assert_eq!(
        s.status(),
        format!(
            "allowed_client_steam_ids=[];available=false;reason={};role=server;service_ready=false",
            text::NOT_INITIALIZED
        )
    );
    // Not registered: available but service_ready false, reason the denial.
    s.net(|f| f.flags = KILN_STEAM_MSG_AVAILABLE | KILN_STEAM_MSG_IDENTITY);
    assert_eq!(
        s.status(),
        format!(
            "allowed_client_steam_ids=[];available=false;reason={};role=server;service_ready=false",
            text::NOT_REGISTERED
        )
    );
    // Denied but settings readable: configured and allowed are still there.
    let c = net(Setup {
        active: false,
        ..CLIENT
    });
    c.shared
        .settings()
        .replace(Ok(json!({"modules": {"network": {
            "serverSteamId": "76561198000000099",
            "allowedClientSteamIds": "76561198000000005"
        }}})));
    c.ports.local.set(Some(11));
    assert_eq!(
        c.status(),
        format!(
            "allowed_client_steam_ids=[76561198000000005];available=false;\
             configured_server_steam_id=76561198000000099;reason={NOT_ACTIVE};role=client;service_ready=false"
        )
    );
    assert_eq!(
        c.calls("net.status"),
        0,
        "a denied status reads no native status"
    );
    // Settings unreadable: both keys absent, the denial is the settings text.
    c.active.0.set(true);
    c.shared.settings().replace(Err("bad".into()));
    assert_eq!(
        c.status(),
        format!(
            "available=false;reason={};role=client;service_ready=false",
            text::SETTINGS_UNAVAILABLE
        )
    );
    // The native status is read again on every call.
    c.shared
        .settings()
        .replace(Ok(network::settings::default_document()));
    let before = c.calls("net.status");
    let _ = c.status();
    let _ = c.status();
    assert_eq!(c.calls("net.status") - before, 4, "denial + native, twice");
}

// ---- provider facts and runtime.has / runtime.status ----

#[test]
fn the_network_facts_follow_the_lease_flags_and_its_capability_row() {
    let _serial = serial();
    let n = net(SERVER);
    let has = |op: &str| {
        n.eval::<bool>(&format!("return runtime.has('{op}')"))
            .unwrap()
    };
    let status = |op: &str| -> (bool, Option<String>) {
        n.eval(&format!(
            "local s = runtime.status('{op}'); return s.available, s.reason"
        ))
        .unwrap()
    };
    let ops = [
        "runtime.network.status",
        "runtime.network.send",
        "runtime.network.receive",
        "runtime.network.accept",
        "runtime.network.send_mod",
        "runtime.network.receive_mod",
        "runtime.network.connected_peers",
    ];
    for op in ops {
        assert!(has(op), "{op}");
        assert_eq!(status(op), (true, None), "{op}");
    }
    for (flags, row) in [
        (KILN_STEAM_MSG_AVAILABLE, KILN_CAP_AVAILABLE),
        (KILN_STEAM_MSG_REGISTERED, KILN_CAP_AVAILABLE),
        (
            KILN_STEAM_MSG_AVAILABLE | KILN_STEAM_MSG_REGISTERED,
            KILN_CAP_PENDING,
        ),
    ] {
        n.net(|f| f.flags = flags);
        fake_services::set_cap(CAPABILITY, Some((row, KILN_CAP_REASON_NONE)));
        for op in ops {
            assert!(!has(op), "{op} {flags} {row}");
            assert_eq!(
                status(op),
                (false, Some(PROVIDER.to_owned())),
                "{op} {flags} {row}"
            );
        }
    }
    // An unavailable row (no Steam in this process) is outside this build's support.
    fake_services::set_cap(
        CAPABILITY,
        Some((KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING)),
    );
    assert_eq!(
        status("runtime.network.send"),
        (false, Some(unverified("runtime.network.send")))
    );
    // Every network operation has landed: with the row available again, all are.
    fake_services::set_cap(CAPABILITY, Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)));
    n.net(|f| f.flags = KILN_STEAM_MSG_AVAILABLE | KILN_STEAM_MSG_REGISTERED);
    for op in ops {
        assert_eq!(status(op), (true, None), "{op}");
    }
    assert_eq!(
        Op::from_name("runtime.network.send").unwrap(),
        Op::NetworkSend
    );
}

// ---- the production family through the worker ----

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
const SOURCE: &str = r#"
local names = {}
for k, v in pairs(runtime.network) do names[#names + 1] = k .. ':' .. type(v) end
table.sort(names)
record('top', table.concat(names, ','), tostring(runtime.network.status().reason))
return {
  on_load = function()
    local ok, reason = runtime.network.send('76561198000000042', 'hello', { channel = 9 })
    local messages, why = runtime.network.receive({ channel = 4 })
    record('load', tostring(ok), tostring(reason), tostring(#messages), tostring(why),
      tostring(runtime.has('runtime.network.send')), tostring(runtime.network.status().available))
    local mod_ok, mod_reason = runtime.network.send_mod('76561198000000042', 'b', 'hi')
    local mail, mail_why = runtime.network.receive_mod()
    local got = {}
    for _, m in ipairs(mail) do got[#got + 1] = m.from_mod .. '>' .. m.to_mod .. ':' .. m.payload end
    record('mod', tostring(mod_ok), tostring(mod_reason), table.concat(got, ','), tostring(mail_why),
      tostring(runtime.has('runtime.network.receive_mod')))
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
        // The maintenance's startup pass runs before any family install: bind the worker thread
        // for the fakes' owner-less members here already.
        fake_services::bind_thread(owner);
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
        // After the installer: the package host is gone again.
        let register: Register = Box::new(move |lua, env, package, ports| {
            note(
                owner,
                format!(
                    "after:{}:{}",
                    package.id(),
                    ports.runtime.package_host().is_none()
                ),
            );
            let id = package.id().to_owned();
            env.set(
                "record",
                lua.create_function(move |_, parts: mlua::Variadic<String>| {
                    note(owner, format!("{id}:{}", parts.join("|")));
                    Ok(())
                })?,
            )
        });
        Ok(RuntimePlan {
            side: Side::Client,
            version: SourceVersion::Sf179,
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
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        let owner = cx.owner();
        // Binds the worker thread for the fakes' owner-less members, and records what the
        // installer gives a family of the package host.
        let probe = Family::new("probe", &[], move |_, _, package, _, cx| {
            fake_services::bind_thread(owner);
            note(
                owner,
                format!(
                    "family:{}:{}",
                    package.id(),
                    cx.package_host()
                        .map(|h| h.owner_context().id().to_owned())
                        .unwrap_or_default()
                ),
            );
            Ok(())
        });
        Families::new(vec![probe, network::production_family()])
    }
    fn game_build(_: &WorkerContext<'_>) -> String {
        BUILD.to_owned()
    }
}

#[test]
fn the_production_family_runs_through_the_worker_on_the_production_host() {
    let _serial = serial();
    assert_eq!(network::production_family().claims(), CLAIMS);
    let owner = next_owner();
    fake_services::queue(owner, 4, [message(PEER, b"in", true)]);
    fake_services::queue(
        owner,
        mod_channel("a"),
        [message(PEER, &wire("b", "a", "x"), true)],
    );
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<NetworkPlan>)));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
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
        for line in [
            format!("family:{id}:{id}"),
            format!("after:{id}:true"),
            // Top level: the lifecycle is not active yet.
            format!("{id}:top|{members}|{NOT_ACTIVE}"),
        ] {
            assert!(seen.contains(&line), "{line} in {seen:?}");
        }
    }
    // on_load (active): one lease for the runtime-create, the first receive took the message.
    let loads: Vec<&String> = seen.iter().filter(|r| r.contains(":load|")).collect();
    assert_eq!(
        loads,
        [
            &"a:load|true|nil|1|nil|true|true".to_owned(),
            &"b:load|true|nil|0|nil|true|true".to_owned()
        ]
    );
    let mods: Vec<&String> = seen.iter().filter(|r| r.contains(":mod|")).collect();
    assert_eq!(
        mods,
        [
            &"a:mod|true|nil|b>a:x|nil|true".to_owned(),
            &"b:mod|true|nil||nil|true".to_owned()
        ]
    );
    let sent = fake_services::net(owner, |n| n.sent.clone());
    assert_eq!(sent.len(), 4);
    let (raw, mod_sent): (Vec<_>, Vec<_>) = sent.iter().partition(|s| s.channel == 9);
    assert!(
        raw.iter().all(
            |s| (s.peer, s.channel, s.flags, s.data.as_slice()) == (PEER, 9, 40, &b"hello"[..])
        )
    );
    for (s, from) in mod_sent.iter().zip(IDS) {
        assert_eq!(
            (s.peer, s.channel, s.flags, s.data.clone()),
            (PEER, mod_channel("b"), 40, wire(from, "b", "hi"))
        );
    }
    assert_eq!(fake_services::net(owner, |n| n.leases.len()), 1);
    // The maintenance's startup pass opened the lease and set the client's policy (any
    // peer); the orderly stop closed it once, after the Lua stop, which also ends the policy.
    assert_eq!(fake_services::counters(owner).calls("net.close"), 1);
    assert_eq!(
        fake_services::net(owner, |n| n.open_leases()),
        0,
        "the orderly stop closed the lease"
    );
    assert_eq!(
        fake_services::net(owner, |n| (n.policies[0].clone(), n.policy.clone())),
        (
            (KILN_STEAM_ACCEPT_ANY, Vec::new()),
            Some((KILN_STEAM_ACCEPT_NONE, Vec::new()))
        )
    );
    assert_eq!(shared.network_lease().get(), 0, "nothing left for the tick");
}

// ---- mod messages (send_mod, receive_mod, the envelope, the FNV channel) ----

mod mod_text {
    pub use kiln_sfi::runtime::network::mod_messages::*;
}
use mod_text::{PROTOCOL, TARGET_LENGTH, envelope, mod_channel};

#[test]
fn the_mod_channel_is_shroudforge_s_fnv_1a() {
    // Printed by ShroudForge 1.7.9's own mod_channel (network.rs:92-100, compiled verbatim);
    // identical in 1.8.0.
    for (id, channel) in [
        ("a", 61_767),
        ("world-editor", 34_672),
        ("client-addon", 32_262),
        ("mod-369", 20_015),
        ("mod-484", 20_015),
        ("", 40_960),
        ("kiln-auto-loot", 59_221),
        ("\u{e9}t\u{e9}", 34_694),
    ] {
        assert_eq!(mod_channel(id), channel, "{id:?}");
    }
    // Never the raw default 0, never the health channel 65535.
    let mut seen = HashSet::new();
    for n in 0..200_000u32 {
        let channel = mod_channel(&format!("mod-{n}"));
        assert!((1..=65_534).contains(&channel), "{n}: {channel}");
        seen.insert(channel);
    }
    assert!(seen.len() > 60_000, "spread: {}", seen.len());
}

#[test]
fn the_envelope_is_shroudforge_s_compact_json_in_its_key_order() {
    assert_eq!(
        String::from_utf8(envelope("alpha", "world-editor", "hi").unwrap()).unwrap(),
        r#"{"protocol":"shroudforge.mod-message.v1","from_mod":"alpha","to_mod":"world-editor","payload":"hi"}"#
    );
    // serde_json's string escapes: quote, backslash, the C0 controls (short forms where JSON has
    // them), nothing else (not '/', not DEL, not non-ASCII).
    assert_eq!(
        String::from_utf8(
            envelope(
                "a\"b",
                "t\\u",
                "\"\\/\n\r\t\u{8}\u{c}\u{0}\u{1f}\u{7f}\u{e9}\u{1f600}"
            )
            .unwrap()
        )
        .unwrap(),
        "{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"a\\\"b\",\"to_mod\":\"t\\\\u\",\
         \"payload\":\"\\\"\\\\/\\n\\r\\t\\b\\f\\u0000\\u001f\u{7f}\u{e9}\u{1f600}\"}"
    );
    // The same bytes serde_json writes for ShroudForge's json! value (preserve_order).
    let mut map = serde_json::Map::new();
    map.insert("protocol".into(), PROTOCOL.into());
    map.insert("from_mod".into(), "x\u{1}y".into());
    map.insert("to_mod".into(), "\u{e9}".into());
    map.insert("payload".into(), "p\"q".into());
    if map.keys().next().map(String::as_str) == Some("protocol") {
        assert_eq!(
            envelope("x\u{1}y", "\u{e9}", "p\"q").unwrap(),
            serde_json::to_vec(&serde_json::Value::Object(map)).unwrap()
        );
    }
}

#[test]
fn mod_message_arguments_raise_before_the_denial_even_for_a_denied_mod() {
    let _serial = serial();
    let n = net(Setup {
        phase: Phase::Startup,
        ..SERVER
    });
    for (call, message) in [
        (
            "runtime.network.send_mod()",
            "bad argument #1: error converting Lua nil to String",
        ),
        (
            "runtime.network.send_mod('1')",
            "bad argument #2: error converting Lua nil to String",
        ),
        (
            "runtime.network.send_mod('1', {}, 'p')",
            "bad argument #2: error converting Lua table to String",
        ),
        (
            "runtime.network.send_mod('1', 'm')",
            "bad argument #3: error converting Lua nil to String",
        ),
        // The payload is a Rust String: valid UTF-8 only.
        (
            "runtime.network.send_mod('1', 'm', '\\255')",
            "bad argument #3: error converting Lua string to String (invalid utf-8",
        ),
        (
            "runtime.network.send_mod('1', '\\255', 'p')",
            "bad argument #2: error converting Lua string to String (invalid utf-8",
        ),
        (
            "runtime.network.send_mod('1', 'm', true)",
            "bad argument #3: error converting Lua boolean to String",
        ),
        (
            "runtime.network.send_mod('1', 'm', 'p', 5)",
            "bad argument #4: error converting Lua integer to table",
        ),
        // The limit converts to usize first.
        (
            "runtime.network.receive_mod(-1)",
            "bad argument #1: error converting Lua integer to usize (out of range)",
        ),
        (
            "runtime.network.receive_mod('x')",
            "bad argument #1: error converting Lua string to usize",
        ),
        (
            "runtime.network.receive_mod({})",
            "bad argument #1: error converting Lua table to usize",
        ),
        (
            "runtime.network.receive_mod(1e300)",
            "bad argument #1: error converting Lua number to usize (out of range)",
        ),
    ] {
        let error = n.raises(call);
        assert!(error.starts_with(message), "{call}: {error}");
    }
    // Valid types reach the denial; peer, target length and limit are checked after it.
    for call in [
        "runtime.network.send_mod(7, 5, 6)",
        "runtime.network.send_mod('0', '', 'p', {channel = 'x'})",
        "runtime.network.send_mod('1', string.rep('t', 97), '\\0')",
    ] {
        assert_eq!(
            n.ok_reason(call),
            (false, Some(PREGAME.to_owned())),
            "{call}"
        );
    }
    for call in [
        "runtime.network.receive_mod()",
        "runtime.network.receive_mod(0)",
        "runtime.network.receive_mod(33)",
        "runtime.network.receive_mod('3')",
        "runtime.network.receive_mod(-0.5)",
    ] {
        assert_eq!(
            n.pair(call),
            (Value::Nil, Some(PREGAME.to_owned())),
            "{call}"
        );
    }
    assert_eq!(
        n.network_calls(),
        0,
        "a denied call reaches no lease member"
    );
}

#[test]
fn mod_messages_are_gated_as_send_and_receive() {
    let _serial = serial();
    // The catalog's static gate names the operation ShroudForge checks: send's and receive's.
    let n = net(Setup {
        id: "mod-gate",
        ..SERVER
    });
    fake_services::set_cap(CAPABILITY, None);
    assert_eq!(
        n.ok_reason(&format!(
            "runtime.network.send_mod('{PEER_TEXT}', 'm', 'p')"
        )),
        (false, Some(unverified("runtime.network.send")))
    );
    assert_eq!(
        n.pair("runtime.network.receive_mod()"),
        (Value::Nil, Some(unverified("runtime.network.receive")))
    );
    fake_services::set_cap(CAPABILITY, Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)));
    // The disabled warning names runtime.network.send, once for the mod id.
    n.shared
        .settings()
        .replace(Ok(json!({"modules": {"network": {"enabled": false}}})));
    for _ in 0..2 {
        assert_eq!(
            n.ok_reason(&format!(
                "runtime.network.send_mod('{PEER_TEXT}', 'm', 'p')"
            )),
            (false, Some(text::DISABLED.to_owned()))
        );
        assert_eq!(
            n.pair("runtime.network.receive_mod()"),
            (Value::Nil, Some(text::DISABLED.to_owned()))
        );
    }
    let warnings: Vec<String> = logs(n.owner)
        .into_iter()
        .filter(|(_, line)| line.contains(text::DISABLED_WARNING))
        .map(|(_, line)| line)
        .collect();
    assert_eq!(
        warnings,
        [format!(
            "shroudforge::network: {} (mod_id=mod-gate, operation=runtime.network.send)",
            text::DISABLED_WARNING
        )]
    );
    // The rest of the chain as for send/receive: not initialized, then not registered.
    n.shared
        .settings()
        .replace(Ok(network::settings::default_document()));
    for (flags, reason) in [
        (KILN_STEAM_MSG_REGISTERED, text::NOT_INITIALIZED),
        (KILN_STEAM_MSG_AVAILABLE, text::NOT_REGISTERED),
    ] {
        n.net(|f| f.flags = flags);
        assert_eq!(
            n.ok_reason(&format!(
                "runtime.network.send_mod('{PEER_TEXT}', 'm', 'p')"
            )),
            (false, Some(reason.to_owned()))
        );
        assert_eq!(
            n.pair("runtime.network.receive_mod()"),
            (Value::Nil, Some(reason.to_owned()))
        );
    }
    assert_eq!(n.network_calls(), 0);
}

#[test]
fn send_mod_sends_one_envelope_on_the_target_s_channel() {
    let _serial = serial();
    let n = net(CLIENT);
    let expect = |to: &str, payload: &str| envelope("alpha", to, payload).unwrap();
    for (call, to, payload, flags) in [
        ("'world-editor', 'hi'", "world-editor", "hi", 40),
        (
            "'world-editor', 'hi', {reliable = false}",
            "world-editor",
            "hi",
            32,
        ),
        (
            "'world-editor', 'hi', {reliable = 0}",
            "world-editor",
            "hi",
            40,
        ),
        // Only reliable is read: a bad channel option is ignored.
        ("'a', '', {channel = 70000}", "a", "", 40),
        ("'a', 'x', {channel = 'x', limit = {}}", "a", "x", 40),
        // Numbers convert to strings first (Rust String arguments).
        ("5, 7", "5", "7", 40),
        // A NUL inside a target is allowed; any valid UTF-8 payload.
        ("'a\\0b', '\\0\\195\\169\"'", "a\0b", "\0\u{e9}\"", 40),
    ] {
        n.net(|f| f.sent.clear());
        assert_eq!(
            n.ok_reason(&format!("runtime.network.send_mod('{PEER_TEXT}', {call})")),
            (true, None),
            "{call}"
        );
        let sent = n.sent();
        assert_eq!(sent.len(), 1, "{call}");
        assert_eq!(
            (sent[0].peer, sent[0].channel, sent[0].flags),
            (PEER, mod_channel(to), flags),
            "{call}"
        );
        assert_eq!(sent[0].data, expect(to, payload), "{call}");
    }
    // Peer first, then the target's byte length (1..96 bytes, not characters).
    n.net(|f| f.sent.clear());
    for (call, reason) in [
        ("'0', '', 'p'", options_text::PEER_ZERO),
        ("'x', '', 'p'", options_text::PEER_NOT_DECIMAL),
        ("'1', '', 'p'", TARGET_LENGTH),
        ("'1', string.rep('t', 97), 'p'", TARGET_LENGTH),
        // 49 two-byte characters are 98 bytes.
        ("'1', string.rep('\\195\\169', 49), 'p'", TARGET_LENGTH),
    ] {
        assert_eq!(
            n.ok_reason(&format!("runtime.network.send_mod({call})")),
            (false, Some(reason.to_owned())),
            "{call}"
        );
    }
    assert_eq!(n.sent(), [], "nothing sent for a refused argument");
    for target in ["string.rep('t', 96)", "string.rep('\\195\\169', 48)", "'t'"] {
        assert_eq!(
            n.ok_reason(&format!("runtime.network.send_mod('1', {target}, 'p')")),
            (true, None),
            "{target}"
        );
    }
    assert_eq!(n.sent().len(), 3);
    // The sender is the package's own id, whatever the payload claims.
    n.net(|f| f.sent.clear());
    let _ = n.ok_reason(&format!(
        "runtime.network.send_mod('{PEER_TEXT}', 'b', '{{\"from_mod\":\"evil\"}}')"
    ));
    let sent: serde_json::Value = serde_json::from_slice(&n.sent()[0].data).unwrap();
    assert_eq!(sent["from_mod"], "alpha");
    assert_eq!(sent["payload"], "{\"from_mod\":\"evil\"}");
}

#[test]
fn send_mod_checks_the_size_after_serialization_and_maps_each_result() {
    let _serial = serial();
    let n = net(CLIENT);
    let overhead = envelope("alpha", "m", "").unwrap().len();
    let fits = 524_288 - overhead;
    let call = |payload: &str| {
        n.ok_reason(&format!(
            "runtime.network.send_mod('{PEER_TEXT}', 'm', {payload})"
        ))
    };
    assert_eq!(call(&format!("string.rep('z', {fits})")), (true, None));
    assert_eq!(n.sent().last().unwrap().data.len(), 524_288);
    let before = n.calls("net.send");
    for payload in [
        format!("string.rep('z', {})", fits + 1),
        // Escaping counts: each quote is two bytes on the wire.
        format!("string.rep('z', {}) .. '\"'", fits - 1),
        // A payload of exactly 524,288 bytes cannot fit with the envelope.
        "string.rep('z', 524288)".to_owned(),
    ] {
        assert_eq!(
            call(&payload),
            (false, Some("payload exceeds 524288 bytes".to_owned())),
            "{payload}"
        );
    }
    assert_eq!(n.calls("net.send"), before, "too big: no lease call");
    assert_eq!(
        call(&format!("string.rep('z', {}) .. '\"'", fits - 2)),
        (true, None)
    );
    // The raw family's result texts.
    for (send_result, eresult, expected) in [
        (
            KILN_OK,
            3,
            (false, Some("Steam returned EResult 3".to_owned())),
        ),
        (
            KILN_ERR_UNAVAILABLE,
            1,
            (false, Some(text::INTERFACE_UNAVAILABLE.to_owned())),
        ),
        (
            KILN_ERR_INTERNAL,
            1,
            (false, Some(text::INTERFACE_UNAVAILABLE.to_owned())),
        ),
    ] {
        n.net(|f| {
            f.send_result = send_result;
            f.eresult = eresult;
        });
        assert_eq!(call("'p'"), expected, "{send_result} {eresult}");
    }
    n.net(|f| {
        f.send_result = KILN_OK;
        f.eresult = 1;
    });
    fake_services::set_shape(
        "kiln.net",
        Some(Shape::NullAt(std::mem::offset_of!(KilnNetApi, steam_send))),
    );
    let missing = call("'p'");
    fake_services::set_shape("kiln.net", None);
    assert_eq!(missing, (false, Some(text::MODULE_UNAVAILABLE.to_owned())));
}

/// The kept mod messages of a receive_mod call as
/// `peer|from_mod|to_mod|payload-bytes|reliable` lines, and its reason.
fn received_mod(n: &Net, call: &str) -> (Option<Vec<String>>, Option<String>) {
    let (messages, reason): (Option<Table>, Option<String>) =
        n.eval(&format!("return {call}")).unwrap();
    let messages = messages.map(|t| {
        t.sequence_values::<Table>()
            .map(|m| {
                let m = m.unwrap();
                let mut keys: Vec<String> =
                    m.pairs::<String, Value>().map(|p| p.unwrap().0).collect();
                keys.sort();
                assert_eq!(
                    keys,
                    ["from_mod", "payload", "peer_steam_id", "reliable", "to_mod"]
                );
                let payload: mlua::String = m.raw_get("payload").unwrap();
                format!(
                    "{}|{}|{}|{:?}|{}",
                    m.raw_get::<String>("peer_steam_id").unwrap(),
                    m.raw_get::<String>("from_mod").unwrap(),
                    m.raw_get::<String>("to_mod").unwrap(),
                    payload.as_bytes().to_vec(),
                    m.raw_get::<bool>("reliable").unwrap()
                )
            })
            .collect()
    });
    (messages, reason)
}
fn wire(from: &str, to: &str, payload: &str) -> Vec<u8> {
    envelope(from, to, payload).unwrap()
}

#[test]
fn receive_mod_keeps_only_valid_envelopes_for_the_local_id() {
    let _serial = serial();
    let n = net(CLIENT);
    let here = mod_channel("alpha");
    let drops: Vec<&[u8]> = vec![
        b"",
        b"not json",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"to_mod\":\"alpha\",\"payload\":\"p\"}x",
        b"[]",
        b"\"shroudforge.mod-message.v1\"",
        b"null",
        // Wrong or missing protocol.
        b"{\"protocol\":\"shroudforge.mod-message.v2\",\"from_mod\":\"b\",\"to_mod\":\"alpha\",\"payload\":\"p\"}",
        b"{\"from_mod\":\"b\",\"to_mod\":\"alpha\",\"payload\":\"p\"}",
        b"{\"protocol\":[\"shroudforge.mod-message.v1\"],\"from_mod\":\"b\",\"to_mod\":\"alpha\",\"payload\":\"p\"}",
        // Another target (even one on the same channel), a missing or non-string to_mod, case.
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"to_mod\":\"beta\",\"payload\":\"p\"}",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"payload\":\"p\"}",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"to_mod\":1,\"payload\":\"p\"}",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"to_mod\":\"ALPHA\",\"payload\":\"p\"}",
        // A from_mod or payload that is not a string.
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":7,\"to_mod\":\"alpha\",\"payload\":\"p\"}",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":null,\"to_mod\":\"alpha\",\"payload\":\"p\"}",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"to_mod\":\"alpha\",\"payload\":\"p\"}",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"to_mod\":\"alpha\",\"payload\":{}}",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"to_mod\":\"alpha\",\"payload\":5}",
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"to_mod\":\"alpha\"}",
        // Invalid UTF-8 inside a string.
        b"{\"protocol\":\"shroudforge.mod-message.v1\",\"from_mod\":\"b\",\"to_mod\":\"alpha\",\"payload\":\"\xff\"}",
    ];
    // Nested deeper than serde_json's recursion limit (128): dropped, not a crash.
    let mut deep = br#"{"protocol":"shroudforge.mod-message.v1","from_mod":"b","to_mod":"alpha","payload":"p","x":"#.to_vec();
    deep.extend(std::iter::repeat_n(b'[', 200));
    deep.extend(std::iter::repeat_n(b']', 200));
    deep.push(b'}');
    for (index, bytes) in drops.into_iter().chain([deep.as_slice()]).enumerate() {
        queue_many(&n, here, &[message(PEER, bytes, true)]);
        assert_eq!(
            received_mod(&n, "runtime.network.receive_mod(1)"),
            (Some(vec![]), None),
            "drop {index}: {}",
            String::from_utf8_lossy(bytes)
        );
    }
    assert_eq!(n.net(|f| f.inbound.get(&here).map_or(0, |q| q.len())), 0);
    // Kept: extra keys ignored, any key order and whitespace, escapes decoded; duplicate keys
    // keep the last value (serde_json). to_mod is the local id, reliable is Steam's.
    let kept = [
        (wire("beta", "alpha", "hi"), "beta", b"hi".to_vec(), true),
        (
            br#" { "payload" : "\u0000\u00e9" , "extra" : [1, {"a": null}], "to_mod":"alpha", "from_mod":"", "protocol":"shroudforge.mod-message.v1" } "#.to_vec(),
            "",
            vec![0, 0xc3, 0xa9],
            false,
        ),
        (
            br#"{"protocol":"x","protocol":"shroudforge.mod-message.v1","from_mod":"b","to_mod":"alpha","payload":"one","payload":"two"}"#.to_vec(),
            "b",
            b"two".to_vec(),
            true,
        ),
        (wire("b", "alpha", &"q".repeat(400_000)), "b", vec![b'q'; 400_000], true),
    ];
    for (bytes, from, payload, reliable) in kept {
        queue_many(&n, here, &[message(PEER, &bytes, reliable)]);
        assert_eq!(
            received_mod(&n, "runtime.network.receive_mod()"),
            (
                Some(vec![format!("{PEER}|{from}|alpha|{payload:?}|{reliable}")]),
                None
            ),
            "{}",
            String::from_utf8_lossy(&bytes[..bytes.len().min(200)])
        );
    }
    // Only the local mod's channel is polled.
    queue_many(&n, 0, &[message(PEER, &wire("b", "alpha", "raw"), true)]);
    queue_many(
        &n,
        mod_channel("beta"),
        &[message(PEER, &wire("b", "alpha", "other"), true)],
    );
    let before = n.calls("net.receive");
    assert_eq!(
        received_mod(&n, "runtime.network.receive_mod()"),
        (Some(vec![]), None)
    );
    assert_eq!(n.calls("net.receive") - before, 1);
    assert_eq!(
        n.net(|f| (f.inbound[&0].len(), f.inbound[&mod_channel("beta")].len())),
        (1, 1)
    );
}

#[test]
fn receive_mod_counts_dropped_messages_and_returns_what_it_took_on_failure() {
    let _serial = serial();
    let n = net(SERVER);
    let here = mod_channel("alpha");
    let other = PEER + 1;
    n.ports.authorized.borrow_mut().insert(PEER);
    queue_many(
        &n,
        here,
        &[
            message(PEER, b"junk", true),
            message(PEER, &wire("b", "alpha", "1"), true),
            // An unauthorized sender on a server is dropped before the envelope is read.
            message(other, &wire("b", "alpha", "2"), true),
            message(PEER, &wire("b", "alpha", "3"), false),
        ],
    );
    // limit 3: three attempts; the dropped ones still count.
    assert_eq!(
        received_mod(&n, "runtime.network.receive_mod(3)"),
        (Some(vec![format!("{PEER}|b|alpha|[49]|true")]), None)
    );
    assert_eq!(n.calls("net.receive"), 3);
    assert_eq!(
        received_mod(&n, "runtime.network.receive_mod(1.9)"),
        (Some(vec![format!("{PEER}|b|alpha|[51]|false")]), None)
    );
    assert_eq!(n.calls("net.receive"), 4);
    // A client keeps every sender.
    let c = net(CLIENT);
    queue_many(&c, here, &[message(other, &wire("z", "alpha", "c"), true)]);
    assert_eq!(
        received_mod(&c, "runtime.network.receive_mod()"),
        (Some(vec![format!("{other}|z|alpha|[99]|true")]), None)
    );
    // A failure mid-loop: the messages taken so far and the receive text; nothing requeued.
    for failure in [
        Inbound::Result(KILN_ERR_INTERNAL),
        Inbound::Result(KILN_ERR_UNAVAILABLE),
        Inbound::Result(KILN_STEAM_ERR_INVALID_DATA),
        message(0, &wire("b", "alpha", "x"), true),
    ] {
        queue_many(
            &c,
            here,
            &[
                message(PEER, &wire("b", "alpha", "a"), true),
                failure,
                message(PEER, &wire("b", "alpha", "after"), true),
            ],
        );
        assert_eq!(
            received_mod(&c, "runtime.network.receive_mod()"),
            (
                Some(vec![format!("{PEER}|b|alpha|[97]|true")]),
                Some(text::RECEIVE_FAILED.to_owned())
            )
        );
        assert_eq!(
            c.net(|f| f.inbound[&here].len()),
            1,
            "the rest stays queued"
        );
        c.net(|f| f.inbound.clear());
    }
    // A missing receive member: the module text and an empty table.
    fake_services::set_shape(
        "kiln.net",
        Some(Shape::NullAt(std::mem::offset_of!(
            KilnNetApi,
            steam_receive
        ))),
    );
    let missing = received_mod(&c, "runtime.network.receive_mod()");
    fake_services::set_shape("kiln.net", None);
    assert_eq!(
        missing,
        (Some(vec![]), Some(text::MODULE_UNAVAILABLE.to_owned()))
    );
}

#[test]
fn receive_mod_limit_raises_shroudforge_s_text_after_the_denial() {
    let _serial = serial();
    let n = net(CLIENT);
    for limit in ["0", "33", "0.9", "-0.5", "'0'"] {
        assert_eq!(
            n.raises(&format!("runtime.network.receive_mod({limit})")),
            format!("runtime error: {}", options_text::LIMIT_RANGE),
            "{limit}"
        );
    }
    assert_eq!(n.calls("net.receive"), 0);
    let here = mod_channel("alpha");
    for (limit, attempts) in [("1.9", 1), ("32.9", 32), ("nil", 16), ("", 16), ("'5'", 5)] {
        let before = n.calls("net.receive");
        queue_many(
            &n,
            here,
            &vec![message(PEER, &wire("b", "alpha", "m"), true); 40],
        );
        let (messages, _) = received_mod(&n, &format!("runtime.network.receive_mod({limit})"));
        assert_eq!(messages.unwrap().len(), attempts, "{limit}");
        assert_eq!(n.calls("net.receive") - before, attempts as u64, "{limit}");
        n.net(|f| f.inbound.clear());
    }
}

#[test]
fn a_channel_collision_drops_the_other_mod_s_message_as_shroudforge_does() {
    let _serial = serial();
    // mod-369 and mod-484 share channel 20015; Steam's queue is destructive and nothing is
    // requeued, so the first poller drops the other's message.
    let first = net(Setup {
        id: "mod-369",
        ..CLIENT
    });
    assert_eq!(mod_channel("mod-369"), mod_channel("mod-484"));
    let channel = mod_channel("mod-484");
    queue_many(
        &first,
        channel,
        &[
            message(PEER, &wire("x", "mod-484", "for 484"), true),
            message(PEER, &wire("x", "mod-369", "for 369"), true),
        ],
    );
    assert_eq!(
        received_mod(&first, "runtime.network.receive_mod()"),
        (
            Some(vec![format!(
                "{PEER}|x|mod-369|{:?}|true",
                b"for 369".to_vec()
            )]),
            None
        )
    );
    assert_eq!(first.net(|f| f.inbound[&channel].len()), 0, "both taken");
}

#[test]
fn a_mod_message_goes_from_one_package_to_another_through_the_lease() {
    let _serial = serial();
    // Kiln to Kiln over the fake fabric: what send_mod writes is what receive_mod reads.
    let a = net(Setup { id: "a", ..CLIENT });
    let b = net(Setup {
        id: "world-editor",
        ..CLIENT
    });
    assert_eq!(
        a.ok_reason(&format!(
            "runtime.network.send_mod('{PEER_TEXT}', 'world-editor', 'hello \\0 \\195\\169', {{reliable = false}})"
        )),
        (true, None)
    );
    let sent = a.sent().pop().unwrap();
    assert_eq!(sent.channel, 34_672);
    queue_many(
        &b,
        sent.channel,
        &[message(
            PEER,
            &sent.data,
            sent.flags & KILN_STEAM_SEND_RELIABLE != 0,
        )],
    );
    assert_eq!(
        received_mod(&b, "runtime.network.receive_mod()"),
        (
            Some(vec![format!(
                "{PEER}|a|world-editor|{:?}|false",
                "hello \0 \u{e9}".as_bytes().to_vec()
            )]),
            None
        )
    );
}
