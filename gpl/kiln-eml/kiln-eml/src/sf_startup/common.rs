// SPDX-License-Identifier: GPL-3.0-or-later
//! One package's registrar in the SF-dialect startup VM: the GPL `game` builders and the core's one module
//! composition on the SAME Lua instance and the SAME `game` table, before any package code runs.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! - modules/mod-runtime/lua/bindings/mod.rs:66-114 (`register`: one `game` table, `runtime.types = game.types`, the
//!   builtin aliases);
//! - modules/mod-runtime/lua/bindings/game/mod.rs:15-23 (`version`, `assets`, `types`, `guid` on one table);
//! - modules/mod-runtime/lua/eml/v1/create_loader_api.rs:7-47 (`loader`: discovered membership, global features
//!   against the package's capabilities);
//! - modules/mod-runtime/lua/ingame/check_ingame_access.rs (the pregame answers of `runtime`, through the runtime registrar).
//!
//! The order inside one registration:
//! 1. the core's base environment exists (`LuaVm::add_package`: `buffer`, `hasher`, `image`, `game.guid`);
//! 2. the GPL builders extend that `game` table: `game.version` (the selected index's data version) and
//!    `game.assets` (this package's table over the VM's one asset session, with the loaded-resource functions'
//!    pregame answers over the package's runtime registrar host at ShroudForge's position, the attribute catalog);
//! 3. sfi-core's one composition (`compose::install`): `io` (exports below the plan's root only when the pass exports),
//!    `integer`, `game.types` from this module's registry port (the session's per-package table), the runtime registrar's `runtime`
//!    (pregame phase, `runtime.types` the same table as `game.types`) over [`StartupRuntime`], then the common
//!    `shroudforge`/`loader`/logging bindings with the package's frozen settings and the plan's frozen locale;
//! 4. the pregame `loader.features.runtime` (`dll = false`) and `loader.runtime.register_dll` (refuses), SF's answers
//!    in a phase without RUNTIME_DLL;
//! 5. the runtime families SF also builds in its pregame VM (`runtime.ecs`, `.impact`, `.services`, `.overrides`,
//!    `.world`, `.network`, `.patch`, `.functions`, `.values`) with their pregame behaviour, over the same runtime registrar
//!    host (the pregame runtime, [`super::pregame_runtime`]; the attribute members read the asset session's catalog, the attribute catalog).
//!
//! What stays absent, never a successful stand-in: every 1.8-only binding.
use std::{rc::Rc, time::Instant};

use mlua::{Lua, Table, Value};
use sfi_core::{
    bindings::{
        OwnerBindings,
        loader::LoaderSnapshot,
        runtime::{
            Availability, EffectState, RuntimeHost, RuntimeOperation, RuntimeOwnerContext, RuntimeProviderFacts,
        },
    },
    compose::{self, Composition, GameTypes},
    eml::io::ExportConfig,
    host::{Clock, HostContext, LogLevel, LogSink, Package},
    locales::LocaleSource,
};

use super::{
    TypeSession,
    game::{self, assets::AssetSession},
    plan::{RuntimeCatalog, StartupPlan},
    pregame_runtime::{self, PregameRuntime},
    resource_values::ValueObjects,
};

/// The VM-wide state every package's registration shares: the one type session, the one asset session and the
/// discovered membership.
pub(crate) struct Shared<'p> {
    pub(crate) plan: &'p StartupPlan,
    pub(crate) types: TypeSession,
    pub(crate) assets: AssetSession,
    pub(crate) discovered: LoaderSnapshot,
    pub(crate) locale: Rc<dyn LocaleSource>,
    /// The value wrappers' production seam (the one the VM's ports and the asset session use), for `runtime.values`.
    pub(crate) objects: ValueObjects,
}

/// The registrar body for `LuaVm::add_package` (see the module docs for the order).
pub(crate) fn register(
    lua: &Lua,
    env: &Table,
    package: &Package,
    context: &HostContext,
    shared: &Shared<'_>,
) -> mlua::Result<()> {
    let manifest = shared
        .plan
        .manifest(package.id())
        .ok_or_else(|| mlua::Error::runtime(format!("registered package is not discovered: {}", package.id())))?
        .clone();
    let game: Table = env
        .raw_get("game")
        .map_err(|_| mlua::Error::runtime("the base environment has no game table"))?;
    game.raw_set("version", shared.types.target().game_version.as_str())?;
    let features = shared.plan.features();
    let runtime: Rc<dyn RuntimeHost> = Rc::new(StartupRuntime::new(
        RuntimeOwnerContext::new(
            &manifest,
            context,
            features.patch,
            features.export,
            shared.plan.catalog().game_build(),
        ),
        shared.plan.catalog().clone(),
    ));
    let loaded = runtime.clone();
    game::install_assets_with(lua, &game, &shared.assets, move |lua, assets| {
        pregame_runtime::attach_loaded_resources(lua, assets, loaded)
    })?;
    let composed = compose::install(
        lua,
        env,
        package,
        context,
        Composition {
            owner: OwnerBindings {
                manifest,
                locale: shared.locale.clone(),
                loader: shared.discovered.clone(),
            },
            exports: ExportConfig {
                enabled: features.export,
                root: package.export_root.clone(),
            },
            game_types: Some(Rc::new(StartupTypes(shared.types.clone()))),
            runtime: Some(runtime.clone()),
        },
    )?;
    // One game table: the composition extended the table the builders wrote, and runtime.types IS game.types.
    let types: Value = composed.game.raw_get("types")?;
    let runtime_table = match &composed.runtime {
        Some(table) if table.raw_get::<Value>("types")? == types => table.clone(),
        _ => return Err(mlua::Error::runtime("the startup composition split the game table")),
    };
    if composed.game != game {
        return Err(mlua::Error::runtime("the startup composition split the game table"));
    }
    install_pregame_loader_runtime(lua, &composed.bindings.loader, &composed.bindings.loader_features)?;
    pregame_runtime::install(
        lua,
        &game,
        &runtime_table,
        &PregameRuntime {
            host: runtime,
            types: shared.types.clone(),
            objects: shared.objects.clone(),
            assets: shared.assets.clone(),
        },
    )
}

/// SF's pregame `loader.features.runtime` and `loader.runtime` (create_loader_api.rs:22-46): RUNTIME_DLL is an
/// ingame-phase feature only (create_mod_session.rs:75-80), so the pass answers `dll = false` and `register_dll`
/// converts its argument, then refuses with SF's text, whatever the package's capabilities. These are the phase's fixed
/// answers, not native admission facts (the runtime phase owns both).
fn install_pregame_loader_runtime(lua: &Lua, loader: &Table, features: &Table) -> mlua::Result<()> {
    let runtime_features = lua.create_table()?;
    runtime_features.raw_set("dll", false)?;
    features.raw_set("runtime", runtime_features)?;
    let runtime = lua.create_table()?;
    runtime.raw_set(
        "register_dll",
        lua.create_function(|_, _path: String| -> mlua::Result<()> {
            Err(mlua::Error::external(
                "EML runtime DLL support is unavailable in this phase",
            ))
        })?,
    )?;
    loader.raw_set("runtime", runtime)
}

/// The runtime registrar's registry port: each package's own `game.types` table over the VM's one session (one Type per index).
struct StartupTypes(TypeSession);

impl GameTypes for StartupTypes {
    fn types(&self, lua: &Lua, _owner: &Package) -> mlua::Result<Table> {
        game::types::create(lua, &self.0)
    }
}

/// The runtime registrar's host in the pregame phase. The package's Runtime/Patch/Export come from its manifest and the global
/// PATCH/EXPORT from the plan, separately (`RuntimeOwnerContext::new`); static support is the plan's build catalog. No
/// live provider exists before the game: every fact is unknown or not ready, no lifecycle is active and there is no
/// write latch. `report_effect` is refused by the runtime registrar before it reaches this host (it needs the ingame phase).
pub struct StartupRuntime {
    owner: RuntimeOwnerContext,
    catalog: Rc<dyn RuntimeCatalog>,
}

impl StartupRuntime {
    /// The host of one package: its owner context (startup phase) and the build's catalog.
    pub fn new(owner: RuntimeOwnerContext, catalog: Rc<dyn RuntimeCatalog>) -> Self {
        Self { owner, catalog }
    }
}

impl RuntimeHost for StartupRuntime {
    fn owner_context(&self) -> &RuntimeOwnerContext {
        &self.owner
    }
    fn static_support(&self, operation: RuntimeOperation) -> Availability {
        self.catalog.static_support(operation)
    }
    fn provider_facts(&self) -> RuntimeProviderFacts {
        RuntimeProviderFacts::default()
    }
    fn lifecycle_active(&self) -> bool {
        false
    }
    fn write_fault(&self) -> Option<String> {
        None
    }
    fn report_effect(&self, state: EffectState, detail: &str) {
        tracing::warn!(target: "shroudforge", mod_id = %self.owner.id(), state = state.as_str(), %detail,
            "runtime effect reported in the startup pass (ignored)");
    }
}

/// The start's UI locale, read once by the host and answered for the whole attempt (no live config read).
pub(crate) struct FrozenLocale(pub(crate) Option<String>);

impl LocaleSource for FrozenLocale {
    fn locale(&self) -> Option<String> {
        self.0.clone()
    }
}

/// Mod log lines into kiln-eml's own log (kiln.log, kiln/logs/eml.log): `[<package>] <text>` under the target
/// `shroudforge`. Trace and debug are off, as SF's default subscriber has them (a package's `print` is debug).
#[derive(Debug, Default, Clone, Copy)]
pub struct TracingLog;

impl LogSink for TracingLog {
    fn enabled(&self, level: LogLevel) -> bool {
        !matches!(level, LogLevel::Trace | LogLevel::Debug)
    }
    fn write(&self, owner: &str, level: LogLevel, payload: &[u8]) {
        let text = String::from_utf8_lossy(payload);
        match level {
            LogLevel::Trace => tracing::trace!(target: "shroudforge", "[{owner}] {text}"),
            LogLevel::Debug => tracing::debug!(target: "shroudforge", "[{owner}] {text}"),
            LogLevel::Info => tracing::info!(target: "shroudforge", "[{owner}] {text}"),
            LogLevel::Warn => tracing::warn!(target: "shroudforge", "[{owner}] {text}"),
            LogLevel::Error => tracing::error!(target: "shroudforge", "[{owner}] {text}"),
        }
    }
}

/// `shroudforge.clock()`: monotonic seconds since this clock was made. The host makes one per process and hands the
/// same one to every attempt (SF's origin is its process start).
#[derive(Debug, Clone, Copy)]
pub struct MonotonicClock(Instant);

impl MonotonicClock {
    pub fn new() -> Self {
        Self(Instant::now())
    }
}

impl Default for MonotonicClock {
    fn default() -> Self {
        Self::new()
    }
}

impl Clock for MonotonicClock {
    fn seconds(&self) -> f64 {
        self.0.elapsed().as_secs_f64()
    }
}
