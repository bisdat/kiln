// SPDX-License-Identifier: MIT
//! The runtime installer's order, inside `LuaVm::add_package`, before any
//! package code runs (SF `lua/ingame/mod.rs:44-82` and
//! `lua/ingame/functions/mod.rs:9-16`, both pins):
//! 1. `game.types` from its port, then 2. the registrar's `register_runtime` (both done by the
//!    one composition, [`crate::worker::compose`]);
//! 3. the container namespaces ([`CONTAINERS`]), created empty, parents first: the
//!    `runtime` tables ShroudForge builds that the 31 runtime operations pass through, and
//!    `runtime.overrides` (the tracker, claim and target functions of several families)
//!    (1.7.9: `lua/ingame/mod.rs:63-77`, `world/mod.rs:14-87`,
//!    `player_input/submit_building_input.rs:171`; `runtime.player` is the registrar's). The
//!    installer owns them: a family claims names inside one (`runtime.world.context_active`,
//!    `runtime.world.entity.spawn`), never a container itself or a path that holds one, so
//!    several families can fill one container;
//! 4. the family extensions, in the order given, each owning the namespaces it claims:
//!    after each family, the direct members of `runtime`, `game` and every container are
//!    compared with what was there before it ran, and the family is refused when it
//!    replaced or removed one (the registrar's `runtime.status`, `game.types`, a container, an
//!    earlier family's member) or added one that none of its claims starts with;
//! 5. `runtime.functions` with the operation catalog ([`install_function_operations`])
//!    last, so it captures the checked closures the families installed, then the list's other
//!    `runtime.functions` members if it has them ([`Families::with_function_members`]).
//!
//! During step 4 the package's runtime host is reachable as [`RuntimeContext::package_host`]
//! (cleared after, also when a family fails).
//!
//! Without a runtime host there is no `runtime` namespace, so steps 3 and 5 install nothing;
//! families still run (they may extend `game` or `shroudforge`) and see
//! `composed.runtime == None`.

use super::context::RuntimeContext;
use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::{RuntimeHost, install_function_operations};
use sfi_core::compose::Composed;
use sfi_core::host::Package;
use std::rc::Rc;

/// Namespaces no family may claim: the composition's and the installer's own.
pub const RESERVED: [&str; 4] = [
    "game.types",
    "runtime",
    "runtime.functions",
    "runtime.types",
];

/// The container namespaces the installer owns (step 3), parents before children.
pub const CONTAINERS: [&str; 8] = [
    "runtime.ecs",
    "runtime.network",
    "runtime.overrides",
    "runtime.world",
    "runtime.world.building",
    "runtime.world.cursor",
    "runtime.world.entity",
    "runtime.world.voxel",
];

/// One family's registrar body.
pub type FamilyInstall =
    Rc<dyn Fn(&Lua, &Table, &Package, &Composed, &RuntimeContext) -> mlua::Result<()>>;

/// What a family runs outside Lua on the lifecycle host's turns (the reserved lifecycle
/// turn hook), all on the worker thread:
/// - [`created`](Self::created) at runtime-create, before any package is set up;
/// - [`command`](Self::command) for each drained inbox command;
/// - [`turn`](Self::turn) at the start of each worker turn, before the scheduler's on_update dispatch;
/// - [`stopped`](Self::stopped) after the Lua stop of an orderly leave, or when runtime-create
///   failed after `created`. A fast leave calls nothing.
///
/// A panic in a hook is caught and logged by the lifecycle host; it never stops the runtime.
pub trait RuntimeHook {
    fn created(
        &self,
        cx: &crate::worker::WorkerContext<'_>,
        runtime: &RuntimeContext,
        side: sfi_core::host::Side,
    ) {
        let _ = (cx, runtime, side);
    }
    fn command(
        &self,
        cx: &crate::worker::WorkerContext<'_>,
        command: &crate::worker::WorkerCommand,
    ) {
        let _ = (cx, command);
    }
    fn turn(&self, cx: &crate::worker::WorkerContext<'_>) {
        let _ = cx;
    }
    fn stopped(&self, cx: &crate::worker::WorkerContext<'_>) {
        let _ = cx;
    }
}

/// A runtime family extension (...): its name, the namespaces it owns
/// (dotted paths such as `runtime.ecs`), its registrar, and its runtime hook if it has one.
#[derive(Clone)]
pub struct Family {
    name: &'static str,
    claims: Vec<&'static str>,
    install: FamilyInstall,
    hook: Option<Rc<dyn RuntimeHook>>,
}
impl Family {
    pub fn new(
        name: &'static str,
        claims: &[&'static str],
        install: impl Fn(&Lua, &Table, &Package, &Composed, &RuntimeContext) -> mlua::Result<()>
        + 'static,
    ) -> Self {
        Self {
            name,
            claims: claims.to_vec(),
            install: Rc::new(install),
            hook: None,
        }
    }
    /// The family with its runtime hook.
    pub fn with_hook(mut self, hook: Rc<dyn RuntimeHook>) -> Self {
        self.hook = Some(hook);
        self
    }
    pub fn name(&self) -> &'static str {
        self.name
    }
    pub fn claims(&self) -> &[&'static str] {
        &self.claims
    }
}

/// What the installer's step 5 adds to `runtime.functions` after `get_operations` (the package's
/// `runtime` table and host).
pub type FunctionMembers = Rc<dyn Fn(&Lua, &Table, &Rc<dyn RuntimeHost>) -> mlua::Result<()>>;

/// The checked family list of one runtime-create. Every namespace has one owner.
#[derive(Clone, Default)]
pub struct Families {
    list: Vec<Family>,
    functions: Option<FunctionMembers>,
}

fn overlaps(a: &str, b: &str) -> bool {
    let nested = |outer: &str, inner: &str| {
        inner.len() > outer.len()
            && inner.starts_with(outer)
            && inner.as_bytes()[outer.len()] == b'.'
    };
    a == b || nested(a, b) || nested(b, a)
}

impl Families {
    /// Refuses (before any Lua) a family name given twice, a namespace two families
    /// claim (the same path, or one inside the other), a reserved namespace, and a
    /// container or a path that holds one.
    pub fn new(families: Vec<Family>) -> Result<Self, String> {
        for (index, family) in families.iter().enumerate() {
            if families[..index].iter().any(|f| f.name == family.name) {
                return Err(format!(
                    "runtime family {} is registered twice",
                    family.name
                ));
            }
            for claim in &family.claims {
                if claim.is_empty() || claim.split('.').any(str::is_empty) {
                    return Err(format!(
                        "runtime family {}: invalid namespace '{claim}'",
                        family.name
                    ));
                }
                if let Some(reserved) = RESERVED
                    .iter()
                    .find(|r| *r == claim || (**r != "runtime" && overlaps(r, claim)))
                {
                    return Err(format!(
                        "runtime family {} claims {claim}, which the installer owns ({reserved})",
                        family.name
                    ));
                }
                if let Some(container) = CONTAINERS
                    .iter()
                    .find(|c| *c == claim || (overlaps(c, claim) && c.len() > claim.len()))
                {
                    return Err(format!(
                        "runtime family {} claims {claim}, which the installer owns (the container {container}); claim names inside it",
                        family.name
                    ));
                }
                for earlier in families[..index].iter() {
                    if let Some(theirs) = earlier.claims.iter().find(|c| overlaps(c, claim)) {
                        return Err(format!(
                            "namespace {claim} of runtime family {} is already owned by {} ({theirs})",
                            family.name, earlier.name
                        ));
                    }
                }
                if family.claims.iter().filter(|c| overlaps(c, claim)).count() > 1 {
                    return Err(format!(
                        "runtime family {} claims {claim} twice",
                        family.name
                    ));
                }
            }
        }
        Ok(Self {
            list: families,
            functions: None,
        })
    }
    /// The list with its `runtime.functions` members (step 5, after `get_operations`).
    pub fn with_function_members(mut self, members: FunctionMembers) -> Self {
        self.functions = Some(members);
        self
    }
    pub fn iter(&self) -> impl Iterator<Item = &Family> {
        self.list.iter()
    }
    pub fn is_empty(&self) -> bool {
        self.list.is_empty()
    }
    /// The families' runtime hooks, in family order.
    pub fn hooks(&self) -> Vec<Rc<dyn RuntimeHook>> {
        self.list.iter().filter_map(|f| f.hook.clone()).collect()
    }
    /// The families that have a runtime hook. Their state belongs to the process's runtime, not to one VM.
    pub fn hooked(&self) -> Vec<Family> {
        self.list
            .iter()
            .filter(|f| f.hook.is_some())
            .cloned()
            .collect()
    }
    /// This list with each family replaced by the family of the same name in `boot` that has a runtime hook.
    /// A live reload's new VM is installed by the instance runtime-create made, whose hook keeps its state (the
    /// network family's lease and passes) and is never created again.
    pub fn with_boot_hooks(self, boot: &[Family]) -> Self {
        Self {
            list: self
                .list
                .into_iter()
                .map(|family| {
                    boot.iter()
                        .find(|b| b.name == family.name && b.hook.is_some())
                        .cloned()
                        .unwrap_or(family)
                })
                .collect(),
            functions: self.functions,
        }
    }
}

/// A table's direct members (raw), in no particular order.
fn members(table: &Table) -> mlua::Result<Vec<(Value, Value)>> {
    table.pairs::<Value, Value>().collect()
}

/// A family may add direct members of `root` that one of its claims starts with, and
/// never replace or remove one that was there before it ran.
fn check_members(
    family: &Family,
    root: &str,
    before: &[(Value, Value)],
    after: &[(Value, Value)],
) -> mlua::Result<()> {
    let name = |key: &Value| match key {
        Value::String(text) => text.to_string_lossy(),
        other => format!("{other:?}"),
    };
    let refuse = |what: String| {
        Err(mlua::Error::runtime(format!(
            "runtime family {}: {what}",
            family.name
        )))
    };
    for (key, value) in before {
        match after.iter().find(|(k, _)| k == key) {
            Some((_, now)) if now == value => {}
            Some(_) => {
                return refuse(format!(
                    "replaced {root}.{}, which it does not own",
                    name(key)
                ));
            }
            None => {
                return refuse(format!(
                    "removed {root}.{}, which it does not own",
                    name(key)
                ));
            }
        }
    }
    for (key, _) in after {
        if before.iter().any(|(k, _)| k == key) {
            continue;
        }
        let path = format!("{root}.{}", name(key));
        let claimed = matches!(key, Value::String(_))
            && family.claims.iter().any(|claim| {
                *claim == path
                    || (claim.starts_with(path.as_str())
                        && claim.as_bytes().get(path.len()) == Some(&b'.'))
            });
        if !claimed {
            return refuse(format!("added {path} without claiming it"));
        }
    }
    Ok(())
}

/// Step 3: the containers, created empty under the registrar's `runtime` table, parents first.
fn create_containers(lua: &Lua, runtime: &Table) -> mlua::Result<Vec<(&'static str, Table)>> {
    let mut created: Vec<(&'static str, Table)> = Vec::with_capacity(CONTAINERS.len());
    for path in CONTAINERS {
        let (parent, name) = path.rsplit_once('.').expect("a container is below runtime");
        let parent = if parent == "runtime" {
            runtime.clone()
        } else {
            created
                .iter()
                .find(|(p, _)| *p == parent)
                .map(|(_, table)| table.clone())
                .expect("containers are listed parents first")
        };
        if !parent.raw_get::<Value>(name)?.is_nil() {
            return Err(mlua::Error::runtime(format!(
                "runtime installer: {path} exists before the installer creates it"
            )));
        }
        let table = lua.create_table()?;
        parent.raw_set(name, &table)?;
        created.push((path, table));
    }
    Ok(created)
}

/// Steps 3 to 5 for one package, right after the composition (steps 1 and 2).
/// `host` is the runtime host the composition gave the registrar (the same `Rc`), `None`
/// when there is no `runtime` namespace.
pub fn install(
    lua: &Lua,
    env: &Table,
    package: &Package,
    composed: &Composed,
    host: Option<Rc<dyn RuntimeHost>>,
    families: &Families,
    cx: &RuntimeContext,
) -> mlua::Result<()> {
    if host.is_some() != composed.runtime.is_some() {
        return Err(mlua::Error::runtime(
            "runtime installer: the registrar's runtime table and its host must come together",
        ));
    }
    let containers = match &composed.runtime {
        Some(runtime) => create_containers(lua, runtime)?,
        None => Vec::new(),
    };
    // The containers' own tables, as created: a family that replaced one is refused at
    // its parent, and what it added to the original is still checked.
    let roots: Vec<(&str, &Table)> = std::iter::once(("game", &composed.game))
        .chain(
            composed
                .runtime
                .as_ref()
                .map(|runtime| ("runtime", runtime)),
        )
        .chain(containers.iter().map(|(path, table)| (*path, table)))
        .collect();
    // The package's host for its families (RuntimeContext::package_host), cleared again
    // however the loop ends.
    struct ClearHost<'a>(&'a RuntimeContext);
    impl Drop for ClearHost<'_> {
        fn drop(&mut self) {
            self.0.set_package_host(None);
        }
    }
    cx.set_package_host(host.clone());
    let clear = ClearHost(cx);
    for family in families.iter() {
        let before = roots
            .iter()
            .map(|(_, table)| members(table))
            .collect::<mlua::Result<Vec<_>>>()?;
        (family.install)(lua, env, package, composed, cx)
            .map_err(|e| mlua::Error::runtime(format!("runtime family {}: {e}", family.name)))?;
        for ((root, table), before) in roots.iter().zip(before) {
            check_members(family, root, &before, &members(table)?)?;
        }
    }
    drop(clear);
    if let (Some(runtime), Some(host)) = (&composed.runtime, host) {
        if !runtime.raw_get::<Value>("functions")?.is_nil() {
            return Err(mlua::Error::runtime(
                "runtime.functions is installed last by the runtime installer",
            ));
        }
        let functions = lua.create_table()?;
        install_function_operations(lua, &functions, host.clone())?;
        runtime.raw_set("functions", functions)?;
        if let Some(members) = &families.functions {
            members(lua, runtime, &host)?;
        }
    }
    Ok(())
}
