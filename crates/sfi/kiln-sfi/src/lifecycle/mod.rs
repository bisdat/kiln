// SPDX-License-Identifier: MIT
//! The lifecycle host.
//!
//! It is the interpreter worker's runtime: runtime-create, the scheduler turn and
//! the runtime's stop, on the worker thread only.
//! - runtime-create builds one runtime-phase VM, installs the count hook before any
//!   mod code, registers every planned package and starts sfi-core's
//!   `LifecycleRuntime` with this host's `CallbackPort`. SF's 15 s create line is
//!   logged once if it is still running then (ShroudForge 1.7.9
//!   windows-bootstrap/runtime_lifecycle.cpp:97-121).
//! - Each worker turn is one `LifecycleRuntime::turn(dt)`: on_update at 8-1000 ms,
//!   missed periods skipped, the host's `dt` (ShroudForge 1.7.9 update_mod_callbacks.rs:5-83).
//! - A panic out of the turn is SF's "Runtime update failed. Stopping the Lua
//!   runtime" (runtime_lifecycle.cpp:168-171): an interpreter stop, which
//!   leaves in order and never restarts in this process.
//! - The orderly leave is SF's destroy: on_unload for each active package in reverse
//!   order, then the VM closes (`__gc` may run mod code, as SF's does). The fast leave
//!   (exit, revoke, a drain kiln-sfi did not ask for) runs no mod code at all: the
//!   worker forgets this runtime.
//! - At the start of a turn, before the families' turn hooks and the on_update dispatch (ShroudForge's
//!   `update`, update_ingame_runtime.rs:16), the latest live reload/unload request runs through sfi-core's
//!   `ReloadProtocol` and `LifecycleRuntime::{unload, reload}` ([`crate::runtime::reload`]). A reload builds the
//!   new VM with the same watchdog, port, hops and services, over the plan source's rebuild of the frozen admitted
//!   set; the families with a runtime hook keep their boot instance. Its runtime facts (the write-fault latch, the
//!   effect records, the ownership store) are its own, as ShroudForge's reload makes a new `AppState`.
//! - Kiln's own refusals of a reload (not admitted at the start, restart-only, changed on disk) are decided
//!   before anything is unloaded, so a refused reload leaves the running mods alone. kiln-sfi's own stop during a
//!   rebuild keeps the new VM for the orderly leave, which stops it before the old runtime.
//! - When the plan source gives VMs typed values ([`PlanSource::values`]), every VM gets its own
//!   kiln.eml.values arena over its own view of the granted archive generation
//!   ([`VmValues`](crate::runtime::values::VmValues)): made after the VM's providers are attached (with the typed
//!   table port) and before any package is registered, given to the families through the VM's runtime context, and
//!   held beside the VM, never in Lua (it holds the VM). A wake is collected at the end of each turn. The arena is
//!   closed and drained while its VM still lives: at the end of the orderly leave (after every on_unload), for a
//!   reload's old VM right after the swap (its VM then closes), for a new VM that did not become the runtime when the
//!   reload returns, and when a VM's setup fails. The fast leave and a cancellation never close one: it is forgotten
//!   with its VM. An arena that does not drain is logged and keeps its reader and view for good.

pub mod exit;
pub mod hook;
pub mod watchdog;

pub use exit::{DrainStep, close_lease_after_fault, drive_drain};
pub use hook::{HOOK_INSTRUCTIONS, WatchdogExpired, is_stop, watchdog_expired};
pub use watchdog::{NativeCall, Overrun, WATCHDOG_LIMIT, Watchdog, WatchdogPause};

use crate::runtime::values::{FaultSink, ValuesTables, VmValues};
use crate::worker::{
    Host, LifecycleEvent, WORLD_KINDS, WorkerCommand, WorkerContext, WorkerRuntime, WorldLifecycle,
};
use hook::HostPort;
use kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1;
use kiln_abi::*;
use mlua::{Lua, Table};
use sfi_core::{
    bindings::OwnerBindings,
    host::{self as core_host, HostContext, LogLevel, Package, Phase, Side, SourceVersion},
    lifecycle::{
        self, CallbackPort, Cleanup, LifecycleRuntime, Monotonic, OwnerRegistration,
        ReloadProtocol, RuntimeSetup, StopPhase,
    },
    locales::LocaleSource,
    lua::{LuaVm, VmAttachments},
    package::discover::Registry,
};
use std::collections::{BTreeMap, BTreeSet};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::rc::Rc;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

/// SF's line when an update fails (runtime_lifecycle.cpp:169).
pub const UPDATE_FAILED: &str = "Runtime update failed. Stopping the Lua runtime";

/// What the host needs per package environment besides sfi-core's own bindings:
/// the hops, the engine families and tests register through this.
pub struct LuaPorts {
    pub watchdog: Rc<Watchdog>,
    /// Hops for tick-bound Kiln facilities.
    pub hops: Rc<crate::hop::Hops>,
    /// The runtime context (owner, service adapter, hops, watchdog) the runtime
    /// families get. The same one for every package of this runtime-create.
    pub runtime: Rc<crate::runtime::RuntimeContext>,
}

pub type Register = Box<dyn Fn(&Lua, &Table, &Package, &LuaPorts) -> mlua::Result<()>>;

impl LuaPorts {
    /// The ports of a live reload's VM. The same watchdog and hops, and a runtime context over the same owner,
    /// services, hops and watchdog with new runtime facts: ShroudForge's reload builds a new `AppState`
    /// (reload_runtime_packages.rs:44, start_ingame_runtime.rs:38-58), so the old runtime's write-fault latch,
    /// effect records and ownership records do not reach the new VM. Known native resource write
    /// availability survives the VM reload.
    fn for_reload(&self) -> Self {
        let boot = &self.runtime;
        let facts = Rc::new(boot.facts().for_reload());
        Self {
            watchdog: self.watchdog.clone(),
            hops: self.hops.clone(),
            runtime: Rc::new(crate::runtime::RuntimeContext::new(
                boot.owner(),
                boot.services().clone(),
                boot.hops().clone(),
                boot.watchdog().clone(),
                facts,
            )),
        }
    }
}

/// Everything runtime-create needs, built on the worker thread (sources are `Rc`).
/// The launch decision and package discovery fill it in production;
/// tests fill it in memory.
pub struct RuntimePlan {
    pub side: Side,
    pub version: SourceVersion,
    /// The discovered registry; every planned package must be in it.
    pub registry: Registry,
    /// Dependency-first candidates (the plan order).
    pub plan: Vec<String>,
    /// Frozen admission; never inferred here.
    pub admitted: BTreeSet<String>,
    pub cleanup: Rc<dyn Cleanup>,
    pub locale: Rc<dyn LocaleSource>,
    /// One monotonic timeline for the scheduler, `shroudforge.clock` and the
    /// watchdog (production: [`process_clock`]).
    pub clock: Rc<dyn Monotonic>,
    /// Loader feature facts of this phase (`loader.features.patch` / `.export`).
    pub loader_patch: bool,
    pub loader_export: bool,
    /// VM providers, attached before the hook and any package.
    pub attachments: Option<VmAttachments>,
    /// Runs after the runtime installer (families and `runtime.functions`), for tests.
    /// Runtime families register through [`PlanSource::families`] instead.
    pub register: Option<Register>,
    /// The composition's ports and the launch decision's withdrawals.
    pub compose: crate::worker::compose::ComposePlan,
}

/// Where a worker's runtime comes from. [`factory::<P>`] is the worker's
/// `RuntimeFactory` for this plan source.
pub trait PlanSource: 'static {
    /// Runs on the worker thread at runtime-create.
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String>;
    /// The runtime families (the installer's third step), on the worker thread at
    /// runtime-create. None by default.
    fn families(cx: &WorkerContext<'_>) -> Result<crate::runtime::Families, String> {
        let _ = cx;
        Ok(crate::runtime::Families::default())
    }
    /// The game build text of ShroudForge's unverified-operation reason
    /// (`operation <op> has not been verified for game build <build>`), on the worker
    /// thread at runtime-create. ShroudForge prints its archive's game version there;
    /// until a plan source knows it, [`GAME_BUILD_UNKNOWN`](crate::runtime::availability::GAME_BUILD_UNKNOWN).
    fn game_build(cx: &WorkerContext<'_>) -> String {
        let _ = cx;
        crate::runtime::availability::GAME_BUILD_UNKNOWN.to_owned()
    }
    /// The plan of a live reload's new VM, on the worker thread at the turn start that runs the request:
    /// rediscovery and the content check again over what this start admitted, with no sfi.json rows or boot
    /// warnings. The lifecycle host never admits more than runtime-create did and keeps runtime-create's clock.
    fn rebuild(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        Self::plan(cx)
    }
    /// Why package `id` can only be reloaded by restarting the game (startup data, DLLs), if it can't.
    fn restart_only(cx: &WorkerContext<'_>, id: &str) -> Option<String> {
        let _ = (cx, id);
        None
    }
    /// The archive generation the typed values (kiln.eml.values) of each VM are opened over, on the worker
    /// thread at runtime-create and at each live reload's rebuild, right after that VM's plan. Every VM gets its own
    /// arena over its own view of that generation ([`VmValues`]); one that cannot be made fails that VM's setup.
    /// `None` (the default): no arena, and the VM has no typed values (plan sources whose core has no
    /// kiln.eml.values, such as tests').
    fn values(cx: &WorkerContext<'_>) -> Option<KilnEmlArchiveIdentityV1> {
        let _ = cx;
        None
    }
    /// Where the reload request and result live, and the IO thread that reads and writes them:
    /// `<game>/kiln/sfi/runtime` of the frozen plan and the sfi.json hub's thread. `None`: no live reload.
    fn reload_transport(cx: &WorkerContext<'_>) -> Option<crate::runtime::reload::Transport> {
        let game = &cx.shared().frozen_plan()?.game;
        Some(crate::runtime::reload::Transport {
            dir: crate::runtime::reload::runtime_dir(game),
            io: cx.shared().status_hub()?.clone(),
        })
    }
    /// Where this start's UI files are (Kiln.Ui's actions, drained at each turn start): the frozen plan's game
    /// folder, launch and config folder. `None`: no drain.
    fn ui_transport(cx: &WorkerContext<'_>) -> Option<crate::runtime::ui::transport::Transport> {
        crate::runtime::ui::production_transport(cx)
    }
    const HOOK_INSTRUCTIONS: u32 = HOOK_INSTRUCTIONS;
}

/// The plan source's functions a live reload calls after runtime-create.
#[derive(Clone, Copy)]
struct Rebuilder {
    plan: fn(&WorkerContext<'_>) -> Result<RuntimePlan, String>,
    families: fn(&WorkerContext<'_>) -> Result<crate::runtime::Families, String>,
    restart_only: fn(&WorkerContext<'_>, &str) -> Option<String>,
    values: fn(&WorkerContext<'_>) -> Option<KilnEmlArchiveIdentityV1>,
}

/// The worker's `RuntimeFactory` for the lifecycle host.
pub fn factory<P: PlanSource>(cx: &WorkerContext<'_>) -> Result<Box<dyn WorkerRuntime>, String> {
    let plan = P::plan(cx)?;
    let families = P::families(cx)?;
    let game_build = P::game_build(cx);
    let values = P::values(cx);
    let hooks = families.hooks();
    let rebuilder = Rebuilder {
        plan: P::rebuild,
        families: P::families,
        restart_only: P::restart_only,
        values: P::values,
    };
    match create(
        cx,
        plan,
        families,
        game_build,
        values,
        P::HOOK_INSTRUCTIONS,
        rebuilder,
    ) {
        Ok(mut host) => {
            // The UI action drain of each turn
            host.ui = P::ui_transport(cx);
            // The request poll on kiln-sfi's IO thread, once the runtime exists.
            if let Some(transport) = P::reload_transport(cx) {
                let polling = cx
                    .shared()
                    .arc()
                    .is_some_and(|worker| transport.start_poll(&worker));
                if polling {
                    host.reload.transport = Some(transport);
                } else {
                    cx.log(
                        KILN_LOG_WARN,
                        "kiln-sfi: no IO thread for the live reload request; ShroudForge's reload and unload are not available",
                    );
                }
            }
            Ok(Box::new(host) as Box<dyn WorkerRuntime>)
        }
        Err(error) => {
            // A hook that ran at create (the network family's startup pass) lets go of what it holds.
            for hook in &hooks {
                guard_hook(cx, "stop", || hook.stopped(cx));
            }
            Err(error)
        }
    }
}

/// Runs one runtime hook call; a panic is logged and goes no further (the hooks run no Lua).
fn guard_hook(cx: &WorkerContext<'_>, what: &str, call: impl FnOnce()) {
    if catch_unwind(AssertUnwindSafe(call)).is_err() {
        cx.log(
            KILN_LOG_ERROR,
            &format!("kiln-sfi: a runtime family's {what} hook failed (caught panic)"),
        );
    }
}

/// The process's monotonic timeline (one origin across VM replacements).
pub fn process_clock() -> Rc<dyn Monotonic> {
    struct ProcessClock(Instant);
    impl Monotonic for ProcessClock {
        fn now(&self) -> Duration {
            self.0.elapsed()
        }
    }
    static ORIGIN: OnceLock<Instant> = OnceLock::new();
    Rc::new(ProcessClock(*ORIGIN.get_or_init(Instant::now)))
}

struct ScriptClock(Rc<dyn Monotonic>);
impl core_host::Clock for ScriptClock {
    fn seconds(&self) -> f64 {
        self.0.now().as_secs_f64()
    }
}

/// The production mod log sink: lines go to kiln.log under kiln-sfi's owner, tagged with
/// the package (`[<package>] <text>`).
pub fn mod_log(host: Host, owner: KilnOwner) -> Rc<dyn core_host::LogSink> {
    Rc::new(ModLog { host, owner })
}
struct ModLog {
    host: Host,
    owner: KilnOwner,
}
impl core_host::LogSink for ModLog {
    /// ShroudForge's default minimum level, INFO (logging/allows_log_level.rs:3-19, both
    /// pins), with no loader config to lower it: shroudforge.log.trace/debug return before
    /// converting anything (create_mod_api.rs:31-43 in 1.7.9, :32-44 in 1.8.0), and print's
    /// DEBUG line is converted but not written (write_mod_log.rs:8-20). So a mod's print()
    /// output does not reach kiln.log at the default level; that is ShroudForge's own
    /// behaviour and is accepted until Kiln has a logging-filter setting.
    fn enabled(&self, level: LogLevel) -> bool {
        !matches!(level, LogLevel::Trace | LogLevel::Debug)
    }
    fn write(&self, package: &str, level: LogLevel, payload: &[u8]) {
        let level = match level {
            LogLevel::Trace => KILN_LOG_TRACE,
            LogLevel::Debug => KILN_LOG_DEBUG,
            LogLevel::Info => KILN_LOG_INFO,
            LogLevel::Warn => KILN_LOG_WARN,
            LogLevel::Error => KILN_LOG_ERROR,
        };
        let text = format!("[{package}] {}", String::from_utf8_lossy(payload));
        self.host.log(self.owner, level, &text);
    }
}

/// One world kind's sessions as the worker drained them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WorldSession {
    /// The session the worker believes live.
    pub live: Option<u32>,
    /// The newest session known to have ended (0 = none).
    pub ended: u32,
}

/// The worker's view of world sessions from drained `WorldLifecycle` commands.
/// Newer events supersede older ones in the inbox, so a session that started and
/// stopped between two drains arrives as its STOPPING alone: that is accepted and
/// ends every older session too. A STARTED of an already ended session is stale.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WorldSessions([WorldSession; WORLD_KINDS]);
impl WorldSessions {
    pub fn apply(&mut self, event: WorldLifecycle) {
        let Some(world) = self.0.get_mut(event.world as usize) else {
            return;
        };
        match event.event {
            LifecycleEvent::Started => {
                if event.session <= world.ended {
                    return;
                }
                if let Some(live) = world.live
                    && live < event.session
                {
                    world.ended = world.ended.max(live);
                }
                world.live = Some(event.session);
            }
            LifecycleEvent::Stopping => {
                world.ended = world.ended.max(event.session);
                if world.live.is_some_and(|live| live <= event.session) {
                    world.live = None;
                }
            }
        }
    }
    pub fn get(&self, world: usize) -> WorldSession {
        self.0.get(world).copied().unwrap_or_default()
    }
}

pub struct LifecycleHost {
    // Field order is drop order: the VM's typed-value arena (closed and drained while the VM it holds lives), then
    // the runtime (and its VM).
    /// The running VM's typed values (kiln.eml.values), when the plan source gives VMs typed values.
    values: Option<VmValues>,
    runtime: Option<LifecycleRuntime>,
    /// A reload's new VM that kiln-sfi's own stop reached while it was built; the orderly leave stops it, and
    /// it closes with the host (or is forgotten with it on a fast leave).
    abandoned: Option<Abandoned>,
    infra: Infra,
    worlds: WorldSessions,
    /// Mod errors already logged, so each new one is logged once (SF logs each).
    logged: std::cell::RefCell<BTreeMap<String, String>>,
    /// The (active, failed) packages last told to sfi.json.
    told: std::cell::RefCell<(BTreeSet<String>, BTreeSet<String>)>,
    /// The `ownerEpoch` values last told to sfi.json (the active packages' and their VM's).
    told_epochs: std::cell::RefCell<BTreeMap<String, u64>>,
    /// The families' runtime hooks (`RuntimeHook`).
    hooks: Vec<Rc<dyn crate::runtime::install::RuntimeHook>>,
    /// The families of runtime-create that have a hook (a reload installs its VM with them), the packages
    /// runtime-create admitted (a reload never admits more), the plan source's rebuild and the request state.
    hooked: Vec<crate::runtime::Family>,
    boot_admitted: BTreeSet<String>,
    rebuilder: Rebuilder,
    reload: ReloadState,
    /// This start's UI files (the action drain at each turn start), when the plan source gives them.
    ui: Option<crate::runtime::ui::transport::Transport>,
    _registry: Registry,
}

/// The worker's side of the live reload transport.
#[derive(Default)]
struct ReloadState {
    /// sfi-core's last-request token; it lives here, so it survives the runtime's replacement.
    protocol: ReloadProtocol,
    /// The latest request the inbox delivered, run at the next turn start.
    pending: Option<serde_json::Value>,
    transport: Option<crate::runtime::reload::Transport>,
}

/// What runtime-create made that outlives one VM: a reload's VM gets the same watchdog, count-hook port,
/// hops, services and clock (its runtime facts are its own, [`LuaPorts::for_reload`]).
struct Infra {
    port: Rc<HostPort>,
    ports: LuaPorts,
    clock: Rc<dyn Monotonic>,
    instructions: u32,
}

/// A reload's new VM that was never swapped in because kiln-sfi's own stop came while it was built.
struct Abandoned {
    // drop order: its typed-value arena (drained while the VM lives), the runtime (and its VM), then its registry
    values: Option<VmValues>,
    runtime: LifecycleRuntime,
    _registry: Registry,
}

/// One VM's runtime, as [`build`] made it.
struct Built {
    // drop order: the VM's typed-value arena before the runtime (and its VM)
    values: Option<VmValues>,
    runtime: LifecycleRuntime,
    registry: Registry,
    /// The packages admitted after the withdrawals.
    admitted: BTreeSet<String>,
}

fn create(
    cx: &WorkerContext<'_>,
    plan: RuntimePlan,
    families: crate::runtime::Families,
    game_build: String,
    values: Option<KilnEmlArchiveIdentityV1>,
    instructions: u32,
    rebuilder: Rebuilder,
) -> Result<LifecycleHost, String> {
    let clock = plan.clock.clone();
    let watchdog = Rc::new(Watchdog::new(clock.clone()));
    let port = Rc::new(HostPort::new(cx, watchdog.clone(), clock.clone()));
    port.begin_create();
    let interrupt_port = port.clone();
    let hops = Rc::new(crate::hop::Hops::new(
        port.shared_arc(),
        Box::new(move || interrupt_port.interrupt_any()),
        watchdog.clone(),
    ));
    let services = Rc::new(crate::runtime::ServiceAdapter::new(
        *cx.shared().host(),
        watchdog.clone(),
    ));
    // The runtime-create's facts, shared by every package's runtime host.
    let facts = Rc::new(crate::runtime::RuntimeFacts::new(
        services.clone(),
        &crate::runtime::operation_catalog::CATALOG,
        crate::runtime::availability::FactsConfig {
            global_patch: plan.loader_patch,
            global_export: plan.loader_export,
            game_build,
        },
    ));
    let ports = LuaPorts {
        watchdog: watchdog.clone(),
        hops: hops.clone(),
        runtime: Rc::new(crate::runtime::RuntimeContext::new(
            cx.shared().owner(),
            services,
            hops,
            watchdog.clone(),
            facts,
        )),
    };
    // The families' hooks run before any package is set up (the network family's startup
    // pass: ShroudForge's service tick before its mod planning, start_ingame_runtime.rs:62-75).
    let hooks = families.hooks();
    for hook in &hooks {
        guard_hook(cx, "create", || hook.created(cx, &ports.runtime, plan.side));
    }
    let infra = Infra {
        port,
        ports,
        clock,
        instructions,
    };
    let built = build(cx, &infra, plan, &families, true, values)?;
    infra.port.end_create();
    if !built.runtime.interrupted() {
        cx.log(KILN_LOG_INFO, "kiln-sfi: Runtime initialized");
    }
    let host = LifecycleHost {
        values: built.values,
        runtime: Some(built.runtime),
        abandoned: None,
        infra,
        worlds: WorldSessions::default(),
        logged: Default::default(),
        told: Default::default(),
        told_epochs: Default::default(),
        hooks,
        hooked: families.hooked(),
        boot_admitted: built.admitted,
        rebuilder,
        reload: ReloadState::default(),
        ui: None,
        _registry: built.registry,
    };
    host.publish(cx);
    Ok(host)
}

/// One VM with every planned package registered and sfi-core's runtime started on it (runtime-create, or a
/// live reload's new VM when `boot` is false: no boot warnings then, and runtime-create's clock). With `values`,
/// the VM's typed-value arena is made over that generation before any package; when the build fails after
/// that, the arena is closed and drained before the VM goes (it drops first, and it holds the VM).
fn build(
    cx: &WorkerContext<'_>,
    infra: &Infra,
    plan: RuntimePlan,
    families: &crate::runtime::Families,
    boot: bool,
    values: Option<KilnEmlArchiveIdentityV1>,
) -> Result<Built, String> {
    let RuntimePlan {
        side,
        version,
        registry,
        plan: mut order,
        mut admitted,
        cleanup,
        locale,
        clock: _,
        loader_patch,
        loader_export,
        attachments,
        register,
        mut compose,
    } = plan;
    let clock = infra.clock.clone();
    // Withdrawn packages leave the plan before the VM exists: no Lua, no read.
    let lines = crate::worker::compose::withdraw(&mut order, &mut admitted, &compose.withdrawn);
    if boot {
        for line in lines {
            cx.log(KILN_LOG_WARN, &line);
        }
    }
    let mut context = HostContext::new(Phase::Runtime, side, version);
    context.clock = Some(Rc::new(ScriptClock(clock.clone())));
    context.log = Some(mod_log(*cx.shared().host(), cx.shared().owner()));
    let vm = LuaVm::new(context).map_err(|e| format!("Lua VM: {e}"))?;
    // A VM with typed values has the frontend's typed table port (a plan's own port is kept)
    let attachments = match attachments {
        Some(mut attachments) if values.is_some() => {
            attachments
                .typed_tables
                .get_or_insert_with(|| Rc::new(ValuesTables));
            Some(attachments)
        }
        None if values.is_some() => Some(VmAttachments {
            typed_tables: Some(Rc::new(ValuesTables)),
            buffer_resources: None,
        }),
        attachments => attachments,
    };
    if let Some(attachments) = attachments {
        vm.attach(attachments)
            .map_err(|e| format!("VM providers: {e}"))?;
    }
    hook::install(vm.lua(), infra.port.clone(), infra.instructions)
        .map_err(|e| format!("count hook: {e}"))?;
    let reloaded;
    let ports = if boot {
        &infra.ports
    } else {
        reloaded = infra.ports.for_reload();
        &reloaded
    };
    // The VM's typed values, before any package is registered; its families reach them through the VM's own
    // runtime context. Declared after the VM, so on a failure below it drops (closed and drained) first.
    let vm_values = match &values {
        Some(identity) => Some(VmValues::create(
            ports.runtime.services(),
            vm.lua(),
            cx.shared().owner(),
            identity,
            values_faults(cx),
        )?),
        None => None,
    };
    ports
        .runtime
        .set_values(vm_values.as_ref().map(VmValues::values));
    // Each package's lifecycle state, made before its environment so its runtime host
    // reads the same state the lifecycle sets.
    let states: crate::runtime::availability::OwnerStates = Default::default();
    // The registrar's production host wherever the composition installs `runtime`: that
    // needs the `game.types` port. A plan that brings its own host keeps it.
    if compose.runtime.is_none() && compose.game_types.is_some() {
        compose.runtime = Some(crate::runtime::availability::hosts(
            ports.runtime.facts().clone(),
            states.clone(),
        ));
    }
    let snapshot = registry.snapshot();
    let mut owners = BTreeMap::new();
    for id in &order {
        let record = registry
            .get(id)
            .ok_or_else(|| format!("planned package is not discovered: {id}"))?;
        let mut settings = None;
        vm.add_package(record.runtime().clone(), |lua, env, package| {
            let (composed, runtime_host) = crate::worker::compose::install_with_host(
                lua,
                env,
                package,
                vm.context(),
                &compose,
                OwnerBindings {
                    manifest: record.parsed().manifest.clone(),
                    locale: locale.clone(),
                    loader: lifecycle::loader_snapshot(&snapshot, loader_patch, loader_export),
                },
                loader_export,
            )?;
            crate::runtime::install::install(
                lua,
                env,
                package,
                &composed,
                runtime_host,
                families,
                &ports.runtime,
            )?;
            settings = Some(composed.bindings.settings);
            if let Some(register) = &register {
                register(lua, env, package, ports)?;
            }
            Ok(())
        })
        .map_err(|e| format!("package {id}: {e}"))?;
        let state = states.borrow_mut().entry(id.clone()).or_default().clone();
        owners.insert(
            id.clone(),
            OwnerRegistration {
                state,
                settings: settings.expect("bindings installed"),
            },
        );
    }
    let setup = RuntimeSetup {
        plan: order,
        admitted: admitted.clone(),
        owners,
        clock,
        cleanup,
    };
    let runtime = LifecycleRuntime::start_with_port(
        vm,
        &registry,
        setup,
        Some(infra.port.clone() as Rc<dyn CallbackPort>),
    )
    .map_err(|e| format!("runtime-create: {e}"))?;
    Ok(Built {
        values: vm_values,
        runtime,
        registry,
        admitted,
    })
}

/// Kiln faults of a VM's typed values (transport refusals, unreadable replies, an arena that did not drain),
/// in kiln.log under kiln-sfi's owner.
fn values_faults(cx: &WorkerContext<'_>) -> FaultSink {
    let (host, owner) = (*cx.shared().host(), cx.shared().owner());
    Rc::new(move |error| {
        host.log(
            owner,
            KILN_LOG_ERROR,
            &format!("kiln-sfi: typed values: {error}"),
        )
    })
}

impl LifecycleHost {
    fn publish(&self, cx: &WorkerContext<'_>) {
        if let Some(runtime) = &self.runtime {
            let active = runtime.active_ids().len() as u32;
            let failed = runtime.errors().len() as u32;
            cx.shared().set_mods(active, failed);
            let mut logged = self.logged.borrow_mut();
            for (id, error) in runtime.errors() {
                if logged.get(id) != Some(error) {
                    cx.log(KILN_LOG_ERROR, &format!("kiln-sfi: mod '{id}': {error}"));
                    logged.insert(id.clone(), error.clone());
                }
            }
            // sfi.json: whose Lua ran (in memory; the hub's thread writes it). A package that
            // was active once stays active in the record to the end, even when it fails later, so
            // the next start knows it ran (the save-sensitive turned-off check); one that failed
            // before it ever ran is `failed`.
            if let Some(hub) = cx.shared().status_hub() {
                let mut told = self.told.borrow_mut();
                let mut active = told.0.clone();
                active.extend(runtime.active_ids());
                let failed: BTreeSet<String> = runtime
                    .errors()
                    .keys()
                    .filter(|id| !active.contains(*id))
                    .cloned()
                    .collect();
                if told.0 != active || told.1 != failed {
                    hub.update(|record| {
                        let mut change = crate::state::Change::None;
                        for id in &active {
                            change = change.max(record.set_lua(id, crate::state::Lua::Active));
                        }
                        for id in &failed {
                            change = change.max(record.set_lua(id, crate::state::Lua::Failed));
                        }
                        change
                    });
                    *told = (active, failed);
                }
                // Each active package's owner epoch (its VM generation's), and none for the others
                let epoch = crate::runtime::ui::transport::owner_epoch(runtime.vm().lua());
                let epochs: BTreeMap<String, u64> = runtime
                    .active_ids()
                    .into_iter()
                    .map(|id| (id, epoch))
                    .collect();
                let mut told_epochs = self.told_epochs.borrow_mut();
                if *told_epochs != epochs {
                    hub.update(|record| {
                        let mut change = crate::state::Change::None;
                        for id in told_epochs.keys().filter(|id| !epochs.contains_key(*id)) {
                            change = change.max(record.set_owner_epoch(id, None));
                        }
                        for (id, epoch) in &epochs {
                            change = change.max(record.set_owner_epoch(id, Some(*epoch)));
                        }
                        change
                    });
                    *told_epochs = epochs;
                }
            }
        }
    }
    /// The orderly leave of one runtime: on_unload for each active package in reverse order.
    fn stop_runtime(cx: &WorkerContext<'_>, runtime: &mut LifecycleRuntime) {
        match catch_unwind(AssertUnwindSafe(|| runtime.stop(StopPhase::Normal))) {
            Ok(report) => {
                for (id, error) in &report.failures {
                    cx.log(
                        KILN_LOG_ERROR,
                        &format!("kiln-sfi: mod '{id}' on_unload failed: {error}"),
                    );
                }
            }
            Err(_) => cx.log(
                KILN_LOG_ERROR,
                "kiln-sfi: the runtime's stop failed (caught panic)",
            ),
        }
    }
    /// The orderly leave's end for one VM's typed values, after its on_unloads: the arena closes and drains
    /// while the VM still lives (the VM closes with the host afterwards). Not once the pool's cancellation arrived:
    /// the rest of the leave is fast and the host, arena and VM included, is forgotten.
    fn close_values(cx: &WorkerContext<'_>, values: Option<&VmValues>) {
        let Some(values) = values else {
            return;
        };
        if cx.cancelled() {
            return;
        }
        if !values.close() {
            cx.log(
                KILN_LOG_ERROR,
                "kiln-sfi: a VM's typed values did not drain; their reader and archive view are kept for good",
            );
        }
    }
    /// A VM's typed values that leave before the host does (a reload's old VM after the swap, a new VM that
    /// did not become the runtime): closed and drained, then dropped, which lets the VM close. After the pool's
    /// cancellation they are forgotten instead, with their VM.
    fn retire_values(cx: &WorkerContext<'_>, values: Option<VmValues>) {
        let Some(values) = values else {
            return;
        };
        if cx.cancelled() {
            values.forget();
            return;
        }
        Self::close_values(cx, Some(&values));
        drop(values);
    }
    fn stop_hooks(&self, cx: &WorkerContext<'_>) {
        for hook in &self.hooks {
            guard_hook(cx, "stop", || hook.stopped(cx));
        }
    }
    /// At the turn start: the latest request through sfi-core's `ReloadProtocol` (requestId dedupe, ShroudForge's
    /// result document), handed to the IO thread. Returns true when the worker must leave now (a stop or a
    /// cancellation, also one that came during the reload): then no result is written and nothing else runs.
    fn process_reload(&mut self, cx: &WorkerContext<'_>) -> bool {
        let Some(request) = self.reload.pending.take() else {
            return false;
        };
        // Never while stopping, at exit, or after an interpreter stop (no restart).
        if cx.interrupt().is_some() || self.runtime.is_none() {
            return true;
        }
        let mut protocol = std::mem::take(&mut self.reload.protocol);
        let transport = self.reload.transport.take();
        let mut answered = None;
        let mut ran = false;
        let processed = catch_unwind(AssertUnwindSafe(|| {
            protocol.process(
                &request,
                |operation, ids| {
                    ran = true;
                    match operation {
                        "unload" => self.unload(ids),
                        _ => self.reload(cx, ids),
                    }
                },
                |value| {
                    if cx.interrupt().is_some() {
                        return Err(crate::runtime::reload::STOPPING.to_owned());
                    }
                    answered = Some(value.clone());
                    match &transport {
                        Some(transport) if transport.publish(value) => Ok(()),
                        _ => Err("no IO thread for the result".to_owned()),
                    }
                },
            )
        }));
        self.reload.protocol = protocol;
        self.reload.transport = transport;
        if processed.is_err() {
            Self::interpreter_stop(cx, UPDATE_FAILED);
            return true;
        }
        if cx.interrupt().is_some() {
            return true;
        }
        // Whatever the request did (a new VM, an unload, a restore after a refusal), the packages running now are
        // a new generation: their UI requests made before it are dropped, never called.
        if ran && let Some(runtime) = &self.runtime {
            crate::runtime::ui::transport::renew_owner_epoch(runtime.vm().lua());
        }
        if let Some(value) = answered {
            let text = |key: &str| {
                value
                    .get(key)
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    .to_owned()
            };
            cx.log(
                KILN_LOG_INFO,
                &format!(
                    "kiln-sfi: runtime mod request {:?}: {}: {}",
                    text("requestId"),
                    text("status"),
                    text("message")
                ),
            );
        }
        false
    }

    /// ShroudForge's unload (unload_runtime_packages.rs): the named active packages in reverse order, each
    /// one's override cleanup through the overrides port; the first failure restores those already unloaded.
    fn unload(&mut self, ids: &[String]) -> Result<(), String> {
        match self.runtime.as_mut() {
            Some(runtime) => runtime.unload(ids),
            None => Err(crate::runtime::reload::STOPPING.to_owned()),
        }
    }

    /// ShroudForge's reload (reload_runtime_packages.rs): every active package is unloaded, a new VM is built
    /// over the plan source's rebuild and replaces the runtime, or the previous packages are restored.
    ///
    /// Kiln's own refusals are decided before anything is unloaded, over the rebuild's plan: a package to check
    /// that cannot come back whole (one not admitted at the start, one only a restart reloads, one the rebuild
    /// withdrew because its files changed, one the rebuild does not admit) refuses the reload with ShroudForge's
    /// failed-to-activate text, and the running packages are left as they are (no on_unload, no restore). The
    /// packages to check are the requested ones and every package active before (Kiln never drops a running
    /// package silently; ShroudForge checks the requested ones only). ShroudForge's own refusals keep its order:
    /// a failed rediscovery or initialization answers after the unload and restores.
    fn reload(&mut self, cx: &WorkerContext<'_>, requested: &[String]) -> Result<(), String> {
        use crate::runtime::reload::STOPPING;
        let Some(runtime) = self.runtime.as_mut() else {
            return Err(STOPPING.to_owned());
        };
        let previous = runtime.active_ids();
        let (infra, rebuilder, boot, hooked) = (
            &self.infra,
            self.rebuilder,
            &self.boot_admitted,
            &self.hooked,
        );
        // The rebuild's plan and families, taken at once as at runtime-create (what the plan made for its
        // families, the type registry's archive view and the override trackers, goes with this rebuild also when it
        // is refused). Their failures are ShroudForge's and answer after the unload.
        let plan =
            (rebuilder.plan)(cx).map_err(|e| format!("updated mod package discovery failed: {e}"));
        let families = plan.as_ref().ok().map(|_| {
            (rebuilder.families)(cx)
                .map(|families| families.with_boot_hooks(hooked))
                .map_err(|e| format!("updated runtime initialization failed: {e}"))
        });
        if let Ok(plan) = &plan
            && let Some(refusal) = refusal(cx, rebuilder, boot, plan, requested, &previous)
        {
            return Err(refusal);
        }
        let mut prepared = Some((plan, families));
        let mut rebuilt = None;
        let mut abandoned = None;
        let result = runtime.reload(requested, || {
            let (plan, families) = prepared.take().expect("one build per reload");
            if cx.interrupt().is_some() {
                return Err(STOPPING.to_owned());
            }
            let mut plan = plan?;
            let families = families.expect("taken with the plan")?;
            // never more than runtime-create admitted, never a package only a restart reloads
            plan.admitted
                .retain(|id| boot.contains(id) && (rebuilder.restart_only)(cx, id).is_none());
            // The new VM's own typed values (a fresh arena over a fresh view)
            let values = (rebuilder.values)(cx);
            let built = build(cx, infra, plan, &families, false, values)
                .map_err(|e| format!("updated runtime initialization failed: {e}"))?;
            let interrupt = cx.interrupt();
            if interrupt == Some(crate::worker::Interrupt::Cancelled) {
                // The pool's cancellation: the worker leaves fast. The new VM is forgotten, never closed (no
                // __gc, no mod code), and so is its arena (never closed, its view never released).
                std::mem::forget(built.runtime);
                if let Some(values) = built.values {
                    values.forget();
                }
                return Err(STOPPING.to_owned());
            }
            if interrupt.is_some() || built.runtime.interrupted() {
                // kiln-sfi's own stop: the new VM is kept for the orderly leave, which stops it too (on_unload
                // for each package whose on_load ran in it, its arena closes, then the VM closes).
                abandoned = Some(Abandoned {
                    values: built.values,
                    runtime: built.runtime,
                    _registry: built.registry,
                });
                return Err(STOPPING.to_owned());
            }
            rebuilt = Some((built.registry, built.values));
            Ok(built.runtime)
        });
        if abandoned.is_some() {
            self.abandoned = abandoned;
        }
        match rebuilt {
            Some((registry, values)) if result.is_ok() => {
                self._registry = registry;
                // The old VM's arena closes and drains now, while the old VM still lives (the arena holds
                // it; sfi-core let go of it at the swap); dropping the arena then lets the old VM close.
                let old = std::mem::replace(&mut self.values, values);
                Self::retire_values(cx, old);
            }
            // The new VM was built and then not taken (a requested package did not become active in it): sfi-core
            // stopped it; its arena closes now, before the VM goes.
            Some((_, values)) => Self::retire_values(cx, values),
            None => {}
        }
        if result.is_ok() {
            // A new runtime: its mod errors are logged again, as ShroudForge logs each start's.
            self.logged.borrow_mut().clear();
        }
        result
    }

    /// The runtime's own fatal failure. kiln-sfi's stop; the worker then
    /// leaves in order and the tick drains the task owner. No restart.
    fn interpreter_stop(cx: &WorkerContext<'_>, why: &str) {
        cx.shared().note_detail(why);
        cx.log(KILN_LOG_ERROR, &format!("kiln-sfi: {why}"));
        cx.shared().request_stop();
    }
}

/// Kiln's own refusal of a reload, decided over the rebuild's plan before anything is unloaded. The packages
/// to check are `requested` and `previous` (the active ones); the first that cannot come back whole answers
/// ShroudForge's `updated runtime mod '<id>' failed to activate: <reason>`.
fn refusal(
    cx: &WorkerContext<'_>,
    rebuilder: Rebuilder,
    boot: &BTreeSet<String>,
    plan: &RuntimePlan,
    requested: &[String],
    previous: &[String],
) -> Option<String> {
    use crate::runtime::reload::{NOT_ADMITTED, failed_to_activate};
    let withdrawn: BTreeMap<&str, &str> = plan
        .compose
        .withdrawn
        .iter()
        .map(|w| (w.mod_id.as_str(), w.reason.as_str()))
        .collect();
    let mut checked = BTreeSet::new();
    for id in requested.iter().chain(previous) {
        if !checked.insert(id) {
            continue;
        }
        let reason = if !boot.contains(id) {
            // ShroudForge checks only an enabled runtime package; anything else is no refusal
            plan.registry
                .get(id)
                .map(|r| &r.parsed().manifest)
                .filter(|m| m.enabled && m.capabilities.iter().any(|c| c.requires_runtime()))
                .map(|_| NOT_ADMITTED.to_owned())
        } else if let Some(reason) = (rebuilder.restart_only)(cx, id) {
            Some(reason)
        } else if let Some(reason) = withdrawn.get(id.as_str()) {
            Some((*reason).to_owned())
        } else if !plan.admitted.contains(id) {
            Some("updated mod did not become active".to_owned())
        } else {
            None
        };
        if let Some(reason) = reason {
            return Some(failed_to_activate(id, &reason));
        }
    }
    None
}

impl WorkerRuntime for LifecycleHost {
    fn command(&mut self, cx: &WorkerContext<'_>, command: WorkerCommand) {
        // World lifecycle is tracked here (and feeds hops and the engine
        // families). Configuration observations wait for their source; UI,
        // input and network commands for theirs.
        // The latest request waits for the next turn start.
        if let WorkerCommand::ReloadRequest(request) = command {
            self.reload.pending = Some(request.request);
            return;
        }
        if let WorkerCommand::WorldLifecycle(event) = &command {
            self.worlds.apply(*event);
            let world = event.world as usize;
            cx.shared().set_world(world, self.worlds.get(world).live);
        }
        for hook in &self.hooks {
            guard_hook(cx, "command", || hook.command(cx, &command));
        }
    }

    fn turn(&mut self, cx: &WorkerContext<'_>, dt: Duration) {
        if self.runtime.is_none() {
            return;
        }
        // ShroudForge's update runs the reload request before its status pass and the callbacks
        // (update_ingame_runtime.rs:16).
        if self.process_reload(cx) {
            return;
        }
        let Some(runtime) = self.runtime.as_mut() else {
            return;
        };
        // The families' turn hooks before the on_update dispatch (ShroudForge's network
        // maintenance before update_mod_callbacks, update_ingame_runtime.rs:17-48).
        for hook in &self.hooks {
            guard_hook(cx, "turn", || hook.turn(cx));
        }
        // Kiln.Ui's UI actions before the on_update dispatch (ShroudForge's dispatch_ui_actions,
        // update_ingame_runtime.rs:47), on this worker thread; each callback inside the port's bracket
        if let Some(ui) = &self.ui {
            let port: &dyn CallbackPort = self.infra.port.as_ref();
            let drained = catch_unwind(AssertUnwindSafe(|| {
                let active = runtime.active_ids();
                crate::runtime::ui::actions::dispatch(
                    runtime.vm().lua(),
                    &active,
                    ui,
                    Some(port),
                    |line| cx.log(KILN_LOG_ERROR, line),
                );
            }));
            if drained.is_err() {
                Self::interpreter_stop(cx, UPDATE_FAILED);
                return;
            }
        }
        let turned = catch_unwind(AssertUnwindSafe(|| runtime.turn(dt.as_secs_f64(), &[])));
        if turned.is_err() {
            Self::interpreter_stop(cx, UPDATE_FAILED);
        }
        self.publish(cx);
        // Closes a wake signalled (a lease or the arena closed off the worker) are collected on the worker
        // at the turn's end (its park ignores unparks, so the turn is where it notices)
        if let Some(values) = &self.values {
            values.collect_if_woken();
        }
    }

    fn stop(&mut self, cx: &WorkerContext<'_>) {
        // The orderly leave is the stop kiln-sfi asked for: on_unload may run now.
        self.infra.port.leaving.set(true);
        // A reload's new VM the stop reached while it was built leaves first (it is the newer). It stays in the
        // host and closes with it, so a cancellation during this leave forgets it with the rest.
        if let Some(abandoned) = self.abandoned.as_mut() {
            Self::stop_runtime(cx, &mut abandoned.runtime);
            Self::close_values(cx, abandoned.values.as_ref());
        }
        let Some(runtime) = self.runtime.as_mut() else {
            self.stop_hooks(cx);
            return;
        };
        Self::stop_runtime(cx, runtime);
        // After the Lua stop, as ShroudForge's destroy shuts its network service down.
        self.stop_hooks(cx);
        self.publish(cx);
        // At the end of the stop, after every on_unload: the arena closes and drains while the VM lives
        Self::close_values(cx, self.values.as_ref());
        cx.log(KILN_LOG_INFO, "kiln-sfi: Runtime stopped");
    }
}
