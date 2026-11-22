// SPDX-License-Identifier: MIT
// the interpreter's runtime.world session and context facts.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins unless a line says otherwise): modules/mod-runtime/lua/ingame/world/
// read_world_context.rs:3-61 (operation_available, session_id, context_active, context_kind), world/mod.rs:14-31 in
// 1.7.9 (the four names; 1.8.0 adds a 1.8-only camera layout line first), lua/ingame/check_ingame_access.rs:386-414
// (runtime_denial_reason), game-runtime/native/engine-context/track_world_context.cpp:157-232 (OperationAvailable,
// ActiveContextAvailable, ContextKind: 1 direct, 2 client-read-only, 0 unknown), native/ecs/
// bind_world_entities.cpp:288-302 in 1.7.9, :292-306 in 1.8.0 (the session marker: 0 without a bound live world),
// lua/shroudforge/v1/definitions/runtime_api.lua:820-838 in 1.7.9, :829-847 in 1.8.0 (the contract).
//! `runtime.world.operation_available`, `context_active`, `context_kind` and
//! `session_id`.
//!
//! Each answers as ShroudForge's does, its denial first (the registrar's `runtime_denial_reason`: phase, permission,
//! lifecycle activity, the contract):
//! - `session_id()`: 0 when the denial of `runtime.ecs.query` refuses, else the live session ([`Snapshot`]), 0
//!   without one. `runtime.ecs.query` is in the contract where kiln.world's ECS reads are (`world.ecs_read`);
//!   elsewhere every package gets 0 and ShroudForge's own mods report "waiting" instead of failing.
//! - `context_active()`: `(false, denial)`; `(true, nil)` while a context is live; else
//!   `(false, "active voxel world context is not available")`.
//! - `context_kind()`: "unknown" on the denial of `runtime.world.context.active` or without a live context; else
//!   "direct" (the local authority: a dedicated server, or a client while it hosts) or "client-read-only" (a client
//!   joined to another machine's world).
//! - `operation_available(name)`: the name converts as a string (a number is coerced; nil, a table or a boolean
//!   raise ShroudForge's bad-argument error; invalid UTF-8 raises); false on that name's denial (a name outside the
//!   contract is always denied); else the provider's world-operation fact. `runtime.world.context.active` is a live
//!   context; every other world operation is false until its family lands; no other operation is a world operation.
//!
//! **The live context** ([`WorldContext::read`], one coherent sample) comes from Kiln's own world facts only, with
//! no ECS read and no engine memory:
//! - the session world is the plan side's (`session_world_kind`, the rule every family reuses: the server world
//!   on a server, the client world on a client), and the session is its `kiln.world.game_world_session`;
//! - that generation is sampled before and after two reads of the core's `session` record; any change between them
//!   is no session, as in kiln.world's own world facts;
//! - the core sees world stops (its `lifecycle` flag; the catalog also needs the `world.lifecycle` row), and its
//!   session state names that world: dedicated or host is direct, remote is client-read-only, none is no context;
//!   the world is set and its generation is not 0;
//! - **the worker's lifecycle stream**: the drained `WorldLifecycle` commands, kept in a
//!   [`WorldSessions`] with its bare STOPPING rule (a STOPPING of a session never seen to start ends it and every
//!   older one), have that very session live, so a world counts only once the worker has taken its STARTED;
//! - the lifecycle subscriber's stopping floor (`WorkerShared::world_stopped`, set on the stopping thread before the
//!   STOPPING is queued) is below it, so a world whose STOPPING is being dispatched is no longer live even
//!   while the core still lists it.
//!
//! Known window: kiln.world advances `game_world_session` in its own STOPPING subscriber, which runs before
//! kiln-sfi's; a read between the two sees the advanced value for a world that is about to go. kiln.world's own
//! world facts accept that same value then, and every session-bound member validates its session per call.
//!
//! **Not ECS readiness**: ShroudForge's ECS readiness (`runtime_provider::ready`, the catalog's `Ready` fact) needs the
//! engine's view of the world's entity table, which only kiln.world's `ecs_context_snapshot` reads; the ECS family
//! asks it (`crate::runtime::ecs::ready`). Nothing here calls `ecs_context_snapshot` or any other ECS member.
//!
//! The family ([`family`]) claims the four names ([`CLAIMS`]) inside the installer's `runtime.world` container.
//! Its runtime hook builds the runtime-create's [`WorldContext`] (attached to the runtime facts, so the registrar's
//! `has`, `status` and `get_operations` read the same context) and feeds it the drained lifecycle commands.
//! Every call runs on the worker and calls Kiln members directly through the service adapter: no hop, no
//! wait.

use crate::lifecycle::WorldSessions;
use crate::runtime::availability::RuntimeFacts;
use crate::runtime::context::RuntimeContext;
use crate::runtime::install::{Family, RuntimeHook};
use crate::runtime::overrides::session_world_kind;
use crate::runtime::services::{ServiceAdapter, ServiceError, Unavailable};
use crate::service_member;
use crate::worker::{WorkerCommand, WorkerContext, WorldLifecycle};
use kiln_abi::services::kiln_world::{KILN_WORLD_SERVICE, KILN_WORLD_VERSION, KilnWorldApi};
use kiln_abi::{
    KILN_SESSION_DEDICATED, KILN_SESSION_HOST, KILN_SESSION_REMOTE, KILN_WORLD_CLIENT,
    KILN_WORLD_SERVER,
};
use mlua::{Lua, Table};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use sfi_core::host::Side;
use sfi_core::lua::FunctionArgs;
use std::cell::RefCell;
use std::rc::Rc;

/// The family's name.
pub const FAMILY: &str = "world.context";

/// The names this family adds inside the installer's `runtime.world` container.
pub const CLAIMS: [&str; 4] = [
    "runtime.world.operation_available",
    "runtime.world.context_active",
    "runtime.world.context_kind",
    "runtime.world.session_id",
];

/// The capability row the context needs: the core's world-destroy hook, without which a stop is not seen.
pub const CAPABILITY: &str = "world.lifecycle";

/// `context_active`'s reason without a live context (read_world_context.rs:40).
pub const NOT_ACTIVE: &str = "active voxel world context is not available";

/// What kind of world the live context is (`ContextKind`, track_world_context.cpp:216-226).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ContextKind {
    /// No live context (0).
    Unknown,
    /// The local authority (1): a dedicated server, or a client while it hosts.
    Direct,
    /// A client joined to another machine's world (2): read-only from this process.
    ClientReadOnly,
}
impl ContextKind {
    /// `context_kind`'s text (read_world_context.rs:55-59).
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Unknown => "unknown",
            Self::Direct => "direct",
            Self::ClientReadOnly => "client-read-only",
        }
    }
}

/// One coherent sample of the live context: a session is nonzero exactly when the kind is known.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Snapshot {
    /// The session world's `game_world_session`; 0 without a live context.
    pub session: u64,
    pub kind: ContextKind,
}
impl Snapshot {
    /// No live context.
    pub const NONE: Self = Self {
        session: 0,
        kind: ContextKind::Unknown,
    };
    /// ShroudForge's `world_context_active()` and `world_operation_available("runtime.world.context.active")`.
    pub fn active(&self) -> bool {
        self.kind != ContextKind::Unknown
    }
}

/// The lifecycle subscriber's stopping floor of a world kind: the newest session it saw stopping (0 = none).
pub type StopFloor = Box<dyn Fn(u32) -> u32>;

/// The live world context of one runtime-create (every VM of it: a reload's too), on the worker thread.
pub struct WorldContext {
    services: Rc<ServiceAdapter>,
    side: Side,
    drained: RefCell<WorldSessions>,
    stops: StopFloor,
}

impl WorldContext {
    /// The context of a runtime on `side` (the plan's), over the runtime-create's adapter, with the lifecycle
    /// subscriber's stopping floor.
    pub fn new(services: Rc<ServiceAdapter>, side: Side, stops: StopFloor) -> Self {
        Self {
            services,
            side,
            drained: RefCell::new(WorldSessions::default()),
            stops,
        }
    }
    pub fn side(&self) -> Side {
        self.side
    }
    /// One command of the worker's lifecycle stream, as drained (stale and bare events as [`WorldSessions`] rules).
    pub fn apply(&self, event: WorldLifecycle) {
        self.drained.borrow_mut().apply(event);
    }
    /// The worker's view of world `kind`'s sessions.
    pub fn drained(&self, kind: u32) -> crate::lifecycle::WorldSession {
        self.drained.borrow().get(kind as usize)
    }

    /// The live context now (see the module docs), or [`Snapshot::NONE`].
    pub fn read(&self) -> Snapshot {
        self.read_world(session_world_kind(self.side))
    }

    /// The world ShroudForge's ECS binds now, its local authority's when there is one (1.7.9
    /// engine-context/resolve_engine_owners.cpp:65,133): on a server the server world; on a client the hosted server
    /// world while it hosts (single player included), else the client world (a joined client's replicated copies).
    /// With that world's live session ([`Self::read_world`]'s checks), 0 without one.
    pub fn ecs_world(&self) -> (u32, u64) {
        let server = KILN_WORLD_SERVER as u32;
        if self.side == Side::Client {
            let hosted = self.read_world(server);
            if hosted.active() {
                return (server, hosted.session);
            }
            let client = KILN_WORLD_CLIENT as u32;
            return (client, self.read_world(client).session);
        }
        (server, self.read_world(server).session)
    }

    /// The live context of world kind `world` of this process, with every check of [`Self::read`] (a dedicated
    /// server's world and a host's worlds are direct, a joined client's client world read-only), or
    /// [`Snapshot::NONE`].
    pub fn read_world(&self, world: u32) -> Snapshot {
        let Ok(table) = self
            .services
            .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        else {
            return Snapshot::NONE;
        };
        let Ok(generation) = service_member!(table, KilnWorldApi, game_world_session) else {
            return Snapshot::NONE;
        };
        // SAFETY: kiln_world.h: any thread, lock-free, any world kind.
        let before = unsafe { self.services.call(generation, (world,)) };
        let (Some(first), Some(second)) = (self.services.session(), self.services.session()) else {
            return Snapshot::NONE;
        };
        // SAFETY: as above.
        let after = unsafe { self.services.call(generation, (world,)) };
        let at = world as usize;
        if before != after
            || first.state != second.state
            || first.worlds[at] != second.worlds[at]
            || first.sessions[at] != second.sessions[at]
        {
            return Snapshot::NONE;
        }
        // Without the world-destroy hook a stop is not seen (KilnSessionInfo.lifecycle 0): no session is trusted.
        if first.lifecycle == 0 {
            return Snapshot::NONE;
        }
        let kind = match (self.side, first.state as i32) {
            (Side::Server, KILN_SESSION_DEDICATED) | (Side::Client, KILN_SESSION_HOST) => {
                ContextKind::Direct
            }
            (Side::Client, KILN_SESSION_REMOTE) => ContextKind::ClientReadOnly,
            _ => return Snapshot::NONE,
        };
        let count = first.sessions[at];
        if first.worlds[at] == 0 || before == 0 || count == 0 {
            return Snapshot::NONE;
        }
        // The worker's lifecycle stream has taken this session's STARTED and no STOPPING of it.
        if self.drained(world).live != Some(count) {
            return Snapshot::NONE;
        }
        // Read last: a STOPPING the subscriber saw before the end of the sample counts.
        if (self.stops)(world) >= count {
            return Snapshot::NONE;
        }
        Snapshot {
            session: before,
            kind,
        }
    }
}

/// The context's members: kiln.world's `game_world_session` and the core's `session` (presence only).
pub fn probe(services: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = services.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)?;
    service_member!(table, KilnWorldApi, game_world_session)?;
    if services.has_session() {
        Ok(())
    } else {
        Err(ServiceError {
            service: "kiln core",
            why: Unavailable::Null { member: "session" },
        })
    }
}

/// `runtime.world.operation_available(name)` (read_world_context.rs:3-14).
pub fn operation_available(host: &dyn RuntimeHost, args: &FunctionArgs) -> mlua::Result<bool> {
    let name = args.get::<String>(0)?;
    // A name outside the contract is denied (check_ingame_access.rs:410-413), whatever the earlier checks say.
    let Some(operation) = Op::from_name(&name) else {
        return Ok(false);
    };
    if runtime_denial_reason(host, operation).is_some() {
        return Ok(false);
    }
    Ok(host.provider_facts().world_operation_available(operation))
}

/// `runtime.world.session_id()` (read_world_context.rs:16-26).
pub fn session_id(host: &dyn RuntimeHost, facts: &RuntimeFacts) -> u64 {
    if runtime_denial_reason(host, Op::EcsQuery).is_some() {
        return 0;
    }
    facts.world_snapshot().session
}

/// `runtime.world.context_active()` (read_world_context.rs:28-42).
pub fn context_active(host: &dyn RuntimeHost, facts: &RuntimeFacts) -> (bool, Option<String>) {
    if let Some(reason) = runtime_denial_reason(host, Op::WorldContextActive) {
        return (false, Some(reason));
    }
    let active = facts.world_snapshot().active();
    (active, (!active).then(|| NOT_ACTIVE.to_owned()))
}

/// `runtime.world.context_kind()` (read_world_context.rs:44-60): the denial, or no live context
/// (`world_operation_available("runtime.world.context.active")` false), is "unknown".
pub fn context_kind(host: &dyn RuntimeHost, facts: &RuntimeFacts) -> &'static str {
    if runtime_denial_reason(host, Op::WorldContextActive).is_some() {
        return ContextKind::Unknown.as_str();
    }
    facts.world_snapshot().kind.as_str()
}

/// Puts the four functions into one package's `runtime.world` table; `host` is that package's registrar host, `facts`
/// the runtime-create's.
pub fn install_into(
    lua: &Lua,
    world: &Table,
    host: Rc<dyn RuntimeHost>,
    facts: Rc<RuntimeFacts>,
) -> mlua::Result<()> {
    let h = host.clone();
    world.raw_set(
        "operation_available",
        lua.create_function(move |_, args: FunctionArgs| operation_available(h.as_ref(), &args))?,
    )?;
    let (h, f) = (host.clone(), facts.clone());
    world.raw_set(
        "context_active",
        lua.create_function(move |_, _: FunctionArgs| Ok(context_active(h.as_ref(), &f)))?,
    )?;
    let (h, f) = (host.clone(), facts.clone());
    world.raw_set(
        "context_kind",
        lua.create_function(move |_, _: FunctionArgs| Ok(context_kind(h.as_ref(), &f)))?,
    )?;
    world.raw_set(
        "session_id",
        lua.create_function(move |_, _: FunctionArgs| Ok(session_id(host.as_ref(), &facts)))?,
    )
}

/// The family's state: the runtime-create's context, made by its hook.
#[derive(Default)]
struct Shared {
    context: RefCell<Option<Rc<WorldContext>>>,
}

impl RuntimeHook for Shared {
    /// Before any package: the context on the plan's side, attached to the runtime facts.
    fn created(&self, cx: &WorkerContext<'_>, runtime: &RuntimeContext, side: Side) {
        let stops: StopFloor = match cx.shared().arc() {
            Some(shared) => Box::new(move |kind| shared.world_stopped(kind)),
            // No shared state to read a stop from: no session is taken as live.
            None => Box::new(|_| u32::MAX),
        };
        let world = Rc::new(WorldContext::new(runtime.services().clone(), side, stops));
        runtime.facts().attach_world(world.clone());
        *self.context.borrow_mut() = Some(world);
    }
    /// The worker's lifecycle stream.
    fn command(&self, _: &WorkerContext<'_>, command: &WorkerCommand) {
        if let WorkerCommand::WorldLifecycle(event) = command
            && let Some(world) = self.context.borrow().as_ref()
        {
            world.apply(*event);
        }
    }
}

/// The production family. A package without a `runtime` namespace gets nothing; a reload's VM gets the same
/// context (its runtime facts are attached to it here).
pub fn family() -> Family {
    let shared = Rc::new(Shared::default());
    let hook: Rc<dyn RuntimeHook> = shared.clone();
    Family::new(FAMILY, &CLAIMS, move |lua, _, _, composed, cx| {
        let (Some(runtime), Some(host)) = (&composed.runtime, cx.package_host()) else {
            return Ok(());
        };
        if let Some(world) = shared.context.borrow().clone() {
            cx.facts().attach_world(world);
        }
        let world: Table = runtime.raw_get("world")?;
        install_into(lua, &world, host, cx.facts().clone())
    })
    .with_hook(hook)
}
