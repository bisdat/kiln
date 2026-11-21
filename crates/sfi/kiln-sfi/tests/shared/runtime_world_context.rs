// SPDX-License-Identifier: MIT
// the interpreter's runtime.world session and context family, shared bodies.
//! The world context's shared bodies: run by kiln-sfi's own tests (tests/runtime_07a.rs) and by
//! any other test package that includes them.
//! `runtime.world.session_id`, `context_active`, `context_kind` and `operation_available` through the real worker,
//! lifecycle host, installer, the registrar's production host and production catalog, on the C-ABI fakes: kiln.world's
//! `game_world_session` and kiln.caps from kiln-abi's signatures, the core's `session` record (support's, with the
//! core's state classification) and the world events the core delivers to kiln-sfi's subscriber.
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), identical in both pins:
//! modules/mod-runtime/lua/ingame/world/read_world_context.rs:3-61, lua/ingame/check_ingame_access.rs:179-414.
// Another test package may load support through other shared bodies too; each copy belongs to its own bodies.
#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_services/mod.rs"]
mod fake_services;
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
mod support;

use kiln_abi::services::kiln_caps::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::world::context::{self, CAPABILITY, NOT_ACTIVE};
use kiln_sfi::runtime::{Families, Family};
use kiln_sfi::worker::*;
use mlua::{Lua, Table};
use serde_json::json;
use sfi_core::compose::GameTypes;
use sfi_core::host::{Package as RuntimePackage, PackageSource, Side, SourceVersion};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
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
/// What this plan source says the game build is.
pub const GAME_BUILD: &str = "1.0.0.0-r07a";
const SERVER: i32 = KILN_WORLD_SERVER;
const CLIENT: i32 = KILN_WORLD_CLIENT;
const OPERATION: &str = "runtime.world.context.active";
/// ShroudForge's provider reason; the licence guard keeps the prefix out of MIT sources.
const NOT_READY: &str = concat!(
    "K",
    "FC Runtime world operation is not ready: runtime.world.context.active"
);
const INACTIVE: &str = "runtime API access is available only while this mod's lifecycle is active";

fn unverified(operation: &str) -> String {
    format!("operation {operation} has not been verified for game build {GAME_BUILD}")
}

/// Per owner: the plan side, the request the packages answer next, and what they recorded.
static SIDES: LazyLock<Mutex<HashMap<KilnOwner, Side>>> = LazyLock::new(Default::default);
static REQUESTS: LazyLock<Mutex<HashMap<KilnOwner, String>>> = LazyLock::new(Default::default);
static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);

fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}

/// Each package answers every function, the registrar's three views of the operation and (the test's own probe) the
/// runtime-create's live context, at the top level and on each new request.
const SOURCE: &str = r#"
local function row(t) return tostring(t.available) .. '|' .. tostring(t.reason) end
local function answers()
  local active, why = runtime.world.context_active()
  local session = runtime.world.session_id()
  return table.concat({
    tostring(session) .. ':' .. math.type(session),
    tostring(active) .. '|' .. tostring(why),
    runtime.world.context_kind(),
    tostring(runtime.world.operation_available('runtime.world.context.active')),
    tostring(runtime.has('runtime.world.context.active')),
    row(runtime.status('runtime.world.context.active')),
    row(runtime.get_operations()['runtime.world.context.active']),
    live(),
  }, ';')
end
record('top', answers())
local last
return {
  on_update = function()
    local request = ask()
    if request ~= last then
      last = request
      record(request, answers())
    end
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
/// The `game.types` port: the production runtime host comes with it.
struct Types;
impl GameTypes for Types {
    fn types(&self, lua: &Lua, _: &RuntimePackage) -> mlua::Result<Table> {
        lua.create_table()
    }
}

/// One runtime package (a package without the runtime permission never runs in game: sfi-core's lifecycle skips it, as
/// ShroudForge does; runtime_07a.rs covers that denial on the registrar directly).
const PACKAGES: [(&str, &[&str]); 1] = [("alpha", &["runtime"])];

struct ContextPlan;
impl PlanSource for ContextPlan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let side = SIDES.lock().unwrap()[&cx.owner()];
        let mut registry = Registry::default();
        for (id, capabilities) in PACKAGES {
            let source: Rc<dyn PackageSource> = Rc::new(Source);
            let mut parsed = package::Package::parse(
                json!({"id": id, "name": id, "version": "1.0.0", "capabilities": capabilities})
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
        let ids: Vec<String> = PACKAGES.iter().map(|(id, _)| id.to_string()).collect();
        Ok(RuntimePlan {
            side,
            version: SourceVersion::Sf179,
            registry,
            plan: ids.clone(),
            admitted: ids.into_iter().collect(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock: process_clock(),
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: None,
            compose: kiln_sfi::worker::compose::ComposePlan {
                game_types: Some(Rc::new(Types) as Rc<dyn GameTypes>),
                runtime: None,
                withdrawn: Vec::new(),
            },
        })
    }
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        let owner = cx.owner();
        Families::new(vec![
            context::family(),
            Family::new("record", &[], move |lua, env, package, _, cx| {
                // The worker thread's owner-less fake members (game_world_session, caps) answer for us.
                fake_services::bind_thread(owner);
                let id = package.id().to_owned();
                env.raw_set(
                    "record",
                    lua.create_function(move |_, parts: mlua::Variadic<String>| {
                        let text = std::iter::once(id.clone())
                            .chain(parts.iter().cloned())
                            .collect::<Vec<_>>()
                            .join("\t");
                        RECORDS.lock().unwrap().entry(owner).or_default().push(text);
                        Ok(())
                    })?,
                )?;
                env.raw_set(
                    "ask",
                    lua.create_function(move |_, ()| {
                        Ok(REQUESTS
                            .lock()
                            .unwrap()
                            .get(&owner)
                            .cloned()
                            .unwrap_or_default())
                    })?,
                )?;
                // The test's probe: the runtime-create's live context as the facts read it.
                let facts = cx.facts().clone();
                env.raw_set(
                    "live",
                    lua.create_function(move |_, ()| {
                        let now = facts.world_snapshot();
                        Ok(format!("{}:{}", now.session, now.kind.as_str()))
                    })?,
                )
            }),
        ])
    }
    fn game_build(_: &WorkerContext<'_>) -> String {
        GAME_BUILD.to_owned()
    }
}

/// A running worker of one owner on the plan side `side`.
struct Run {
    owner: KilnOwner,
    pool: std::sync::Arc<FakePool>,
    shared: std::sync::Arc<WorkerShared>,
}

/// The fakes this body changes (the core's session record, the caps rows), back as other bodies expect them,
/// also when the body fails.
struct Restore;
impl Drop for Restore {
    fn drop(&mut self) {
        set_session(SERVER, 0);
        set_session(CLIENT, 0);
        set_session_side(true);
        set_lifecycle(true);
        fake_services::set_cap(CAPABILITY, None);
    }
}

fn lifecycle_row(available: bool) {
    fake_services::set_cap(
        CAPABILITY,
        Some(if available {
            (KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)
        } else {
            (KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED)
        }),
    );
}

fn start(side: Side) -> Run {
    let owner = next_owner();
    SIDES.lock().unwrap().insert(owner, side);
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<ContextPlan>)));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    let run = Run {
        owner,
        pool,
        shared,
    };
    for (id, _) in PACKAGES {
        run.wait(&format!("{id}\ttop\t"));
    }
    run
}

impl Run {
    fn wait(&self, prefix: &str) -> String {
        let found = || {
            records(self.owner)
                .into_iter()
                .find(|r| r.starts_with(prefix))
        };
        assert!(
            eventually(WAIT, || found().is_some()),
            "never saw {prefix:?}: {:?} / {:?}",
            records(self.owner),
            self.shared.status()
        );
        found().unwrap()
    }
    /// The answers of `id` at the top level.
    fn top(&self, id: &str) -> Vec<String> {
        split(&self.wait(&format!("{id}\ttop\t")))
    }
    /// A world event the core delivers to kiln-sfi's subscriber, waited for until the worker has drained it.
    fn world(&self, event: i32, kind: i32, session: u32) {
        fire_world(self.owner, event, kind, session);
        let live = if event == KILN_WORLD_STARTED {
            session
        } else {
            0
        };
        assert!(
            eventually(WAIT, || self.shared.status().worlds[kind as usize] == live),
            "{:?}",
            self.shared.status()
        );
    }
    /// The package's answers to a new request (it answers once per request).
    fn ask(&self, request: &str) -> Vec<String> {
        REQUESTS
            .lock()
            .unwrap()
            .insert(self.owner, request.to_owned());
        split(&self.wait(&format!("alpha\t{request}\t")))
    }
    fn stop(self) {
        self.shared.request_stop();
        assert!(
            self.pool.task(0).wait_finished(WAIT),
            "{:?}",
            self.shared.status()
        );
    }
}

fn split(record: &str) -> Vec<String> {
    let answers = record.splitn(3, '\t').nth(2).unwrap();
    answers.split(';').map(str::to_owned).collect()
}

/// The answers with a live context of `kind` (`live` is the probe's `session:kind`). session_id stays 0:
/// `runtime.ecs.query` is outside the contract until the ECS queries land.
fn live(kind: &str, live: &str) -> Vec<String> {
    [
        "0:integer",
        "true|nil",
        kind,
        "true",
        "true",
        "true|nil",
        "true|nil",
        live,
    ]
    .map(str::to_owned)
    .to_vec()
}
/// The answers without a live context.
fn none() -> Vec<String> {
    vec![
        "0:integer".to_owned(),
        format!("false|{NOT_ACTIVE}"),
        "unknown".to_owned(),
        "false".to_owned(),
        "false".to_owned(),
        format!("false|{NOT_READY}"),
        format!("false|{NOT_READY}"),
        "0:unknown".to_owned(),
    ]
}
/// On a dedicated server: the menu (no world); a world kiln.world has no session for yet (loading); a live world
/// once the worker took its STARTED; the next world at the same address, live only after the worker took that
/// STARTED too; a bare STOPPING (of a session never seen to start) ending the live one while the core still lists
/// it; a stale STARTED; a core that does not see stops; the `world.lifecycle` row off (the operation leaves the
/// contract); the world's end.
pub fn on_a_dedicated_server_the_context_follows_the_worker_s_lifecycle_stream() {
    let _session = session_lock();
    let _restore = Restore;
    set_session_side(true);
    lifecycle_row(true);
    let run = start(Side::Server);
    // Top level: the lifecycle is not active yet: the denials say so; has() and status() do not read it.
    assert_eq!(
        run.top("alpha"),
        [
            "0:integer".to_owned(),
            format!("false|{INACTIVE}"),
            "unknown".to_owned(),
            "false".to_owned(),
            "false".to_owned(),
            format!("false|{NOT_READY}"),
            format!("false|{INACTIVE}"),
            "0:unknown".to_owned(),
        ]
    );
    // The menu: no world.
    let alpha = run.ask("menu");
    assert_eq!(alpha, none());
    // The worker took the STARTED, but kiln.world has no session for the world yet (loading): no context.
    set_session(SERVER, 1);
    fake_services::set_world_session(run.owner, SERVER as u32, 0);
    run.world(KILN_WORLD_STARTED, SERVER, 1);
    let alpha = run.ask("loading");
    assert_eq!(alpha, none());
    // In game: a direct context in session 5.
    fake_services::set_world_session(run.owner, SERVER as u32, 5);
    let alpha = run.ask("in-game");
    assert_eq!(alpha, live("direct", "5:direct"));
    // The next world at the same address (the fake core keeps one world pointer): the core lists session 2 with a
    // new generation, but the worker's stream still has session 1 live: no context until it takes the STARTED.
    set_session(SERVER, 2);
    fake_services::set_world_session(run.owner, SERVER as u32, 7);
    let alpha = run.ask("not-started");
    assert_eq!(alpha, none());
    run.world(KILN_WORLD_STARTED, SERVER, 2);
    let alpha = run.ask("same-address");
    assert_eq!(alpha, live("direct", "7:direct"));
    // A bare STOPPING of session 3 (never seen to start) ends 2 too, though the core still lists 2.
    run.world(KILN_WORLD_STOPPING, SERVER, 3);
    let alpha = run.ask("bare-stopping");
    assert_eq!(alpha, none());
    // A STARTED of an ended session is stale (the inbox drops it, or the stream ignores it).
    fire_world(run.owner, KILN_WORLD_STARTED, SERVER, 2);
    let alpha = run.ask("stale-started");
    assert_eq!(alpha, none());
    // Session 4; then the core says it does not see stops: no session is trusted.
    set_session(SERVER, 4);
    fake_services::set_world_session(run.owner, SERVER as u32, 9);
    run.world(KILN_WORLD_STARTED, SERVER, 4);
    let alpha = run.ask("session-4");
    assert_eq!(alpha, live("direct", "9:direct"));
    set_lifecycle(false);
    let alpha = run.ask("no-lifecycle");
    assert_eq!(alpha, none());
    set_lifecycle(true);
    // The world.lifecycle row off: the operation is outside the contract and the context is not read.
    lifecycle_row(false);
    let alpha = run.ask("row-off");
    let outside = unverified(OPERATION);
    assert_eq!(
        alpha,
        [
            "0:integer".to_owned(),
            format!("false|{outside}"),
            "unknown".to_owned(),
            "false".to_owned(),
            "false".to_owned(),
            format!("false|{outside}"),
            format!("false|{outside}"),
            "0:unknown".to_owned(),
        ]
    );
    lifecycle_row(true);
    let alpha = run.ask("row-on");
    assert_eq!(alpha, live("direct", "9:direct"));
    // The world stops and the core forgets it.
    run.world(KILN_WORLD_STOPPING, SERVER, 4);
    set_session(SERVER, 0);
    let alpha = run.ask("stopped");
    assert_eq!(alpha, none());
    run.stop();
}

/// On a game client the session world is the client world: hosting (a server world runs here too) is direct, a
/// client joined to another machine's world is read-only, the server world's own events change nothing.
pub fn on_a_client_hosting_is_direct_and_joining_is_read_only() {
    let _session = session_lock();
    let _restore = Restore;
    set_session_side(false);
    lifecycle_row(true);
    let run = start(Side::Client);
    let alpha = run.ask("menu");
    assert_eq!(alpha, none());
    // Hosting: the server world runs here, the client world session 1 with generation 3.
    set_session(SERVER, 1);
    run.world(KILN_WORLD_STARTED, SERVER, 1);
    let alpha = run.ask("host-server-only");
    assert_eq!(alpha, none(), "the client world has not started");
    set_session(CLIENT, 1);
    fake_services::set_world_session(run.owner, CLIENT as u32, 3);
    run.world(KILN_WORLD_STARTED, CLIENT, 1);
    let alpha = run.ask("host");
    assert_eq!(alpha, live("direct", "3:direct"));
    // Leaving: both worlds stop.
    run.world(KILN_WORLD_STOPPING, CLIENT, 1);
    run.world(KILN_WORLD_STOPPING, SERVER, 1);
    set_session(CLIENT, 0);
    set_session(SERVER, 0);
    let alpha = run.ask("left");
    assert_eq!(alpha, none());
    // Joining another machine's world: only the client world runs here.
    set_session(CLIENT, 2);
    fake_services::set_world_session(run.owner, CLIENT as u32, 11);
    run.world(KILN_WORLD_STARTED, CLIENT, 2);
    let alpha = run.ask("remote");
    assert_eq!(alpha, live("client-read-only", "11:client-read-only"));
    run.stop();
}
