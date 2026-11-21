// SPDX-License-Identifier: MIT
//! What the worker's runtime installs into each package
//! environment, and the whole-package withdrawals it honours before any Lua runs.
//!
//! The modules come from sfi-core's one composition, [`sfi_core::compose::install`]
//! (the same registrar path the conformance host runs): SF's base environment, `io`,
//! `integer`, `game.types` from the registry port, `runtime` (the runtime registrar) and the common
//! `shroudforge` / `loader` / logging bindings. The ports are the plan's:
//! - no [`GameTypes`] port: no `game.types` and no `runtime`;
//! - with one, the lifecycle host gives the runtime registrar its production runtime host
//!   ([`crate::runtime::availability::hosts`]) unless the plan brings its own.
//!
//! Lua is entered on the worker thread only. Nothing a producer holds can reach it:
//! the VM's handles are `!Send`, so a command cannot carry one.
//! ```compile_fail
//! fn producer<T: Send + 'static>(_: T) {}
//! let lua = mlua::Lua::new();
//! producer(lua.create_table().unwrap()); // mlua handles never leave the worker
//! ```

use sfi_core::{
    bindings::{OwnerBindings, runtime::RuntimeHost},
    compose::{self, Composed, Composition, GameTypes},
    eml::io::ExportConfig,
    host::{HostContext, Package},
    package::Manifest,
};
use sfi_policy::native_selection::Withdrawn;
use std::collections::BTreeSet;
use std::rc::Rc;

/// Builds the runtime host for one package from its manifest and phase context.
pub type RuntimeHosts = Rc<dyn Fn(&Manifest, &HostContext) -> Rc<dyn RuntimeHost>>;

/// The composition inputs of one runtime-create, filled by the launch decision.
#[derive(Clone, Default)]
pub struct ComposePlan {
    /// The runtime registrar's registry port for `game.types`.
    pub game_types: Option<Rc<dyn GameTypes>>,
    /// The runtime host per package. Requires `game_types`. `None` with `game_types`
    /// set: the lifecycle host installs the production one.
    pub runtime: Option<RuntimeHosts>,
    /// Whole packages the frozen launch decision withdrew (`sfi_policy` native
    /// selection, with dependents). They never register and none of their Lua runs.
    pub withdrawn: Vec<Withdrawn>,
}

/// The loud warning for one withdrawn package (a package that
/// cannot run whole is skipped with its reason plus the loud warning).
pub fn warning(withdrawn: &Withdrawn) -> String {
    let mut line = format!(
        "kiln-sfi: WARNING: ShroudForge package '{}' will not run: {}",
        withdrawn.mod_id, withdrawn.reason
    );
    if !withdrawn.caused_by.is_empty() {
        line.push_str(&format!(" (via {})", withdrawn.caused_by.join(" -> ")));
    }
    line
}

/// Removes every withdrawn package from the plan and the frozen admission before the
/// VM exists, and returns one loud warning per withdrawal, in the decision's order.
/// Discovered membership is unchanged: `loader.has_mod` still sees the package.
pub fn withdraw(
    order: &mut Vec<String>,
    admitted: &mut BTreeSet<String>,
    withdrawn: &[Withdrawn],
) -> Vec<String> {
    let ids: BTreeSet<&str> = withdrawn.iter().map(|w| w.mod_id.as_str()).collect();
    order.retain(|id| !ids.contains(id.as_str()));
    admitted.retain(|id| !ids.contains(id.as_str()));
    withdrawn.iter().map(warning).collect()
}

/// The registrar body the lifecycle host runs inside `LuaVm::add_package` for each
/// planned package, before any package code. `export` is the phase's global export
/// switch (the same fact as `loader.features.export`'s global half).
pub fn install(
    lua: &mlua::Lua,
    env: &mlua::Table,
    package: &Package,
    context: &HostContext,
    plan: &ComposePlan,
    owner: OwnerBindings,
    export: bool,
) -> mlua::Result<Composed> {
    install_with_host(lua, env, package, context, plan, owner, export).map(|(composed, _)| composed)
}

/// [`install`], also returning the runtime host the registrar received (the same `Rc`), for
/// the runtime installer's last step (`crate::runtime::install`).
pub fn install_with_host(
    lua: &mlua::Lua,
    env: &mlua::Table,
    package: &Package,
    context: &HostContext,
    plan: &ComposePlan,
    owner: OwnerBindings,
    export: bool,
) -> mlua::Result<(Composed, Option<Rc<dyn RuntimeHost>>)> {
    let runtime = plan
        .runtime
        .as_ref()
        .map(|hosts| hosts(&owner.manifest, context));
    let host = runtime.clone();
    compose::install(
        lua,
        env,
        package,
        context,
        Composition {
            exports: ExportConfig {
                enabled: export,
                root: package.export_root.clone(),
            },
            game_types: plan.game_types.clone(),
            runtime,
            owner,
        },
    )
    .map(|composed| (composed, host))
}
