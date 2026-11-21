// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Both pins, modules/mod-runtime/ingame/: load_runtime_callbacks.rs:13-153,
// activate_runtime_mod.rs:12-43, update_mod_callbacks.rs:5-83,
// unload_runtime_mod.rs:5-21 (1.8), :5-19 (1.7.9), stop_ingame_runtime.rs:5-25,
// plan_runtime_mods.rs:49-78, mod.rs:54-59 (1.7.9), :56-61 (1.8).
use super::{Monotonic, advance_deadline, configuration::AppliedConfiguration};
use crate::{
    host::SourceVersion,
    lua::LuaVm,
    package::{Manifest, discover::Registry},
    settings::Settings,
};
use mlua::{Function, RegistryKey, Table, Value};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, BTreeSet},
    rc::Rc,
    time::Duration,
};

/// Small independent permission cell for the host's eventual RuntimeHost adapter.
/// Querying it from Lua cannot borrow the lifecycle's collection or callback state.
#[derive(Default)]
pub struct OwnerState {
    active: Cell<bool>,
    cleanup_pending: Cell<bool>,
    cleanup_error: RefCell<Option<String>>,
}
impl OwnerState {
    pub fn active(&self) -> bool {
        self.active.get()
    }
    pub fn cleanup_pending(&self) -> bool {
        self.cleanup_pending.get()
    }
    /// Copied diagnostic; querying this never retains a borrow across a host call.
    pub fn cleanup_error(&self) -> Option<String> {
        self.cleanup_error.borrow().clone()
    }
}

/// Owner-scoped restoration supplied by the host. Ordinary restores are immediate
/// caller-thread gated attempts, never jobs, waits or spin loops. Return the source
/// tracker's exact summary (including pending vs inspection fault) unchanged.
/// Source: lua/ingame/overrides/track_runtime_overrides.rs:39-72, both pins.
pub trait Cleanup {
    fn restore(&self, owner: &str) -> Result<(), String>;
    /// 1.8 input reservation release follows the attempted restore even on error.
    fn release_input_reservations(&self, owner: &str);
}

/// The interpreter worker's port. It brackets every
/// mod lifecycle call (module top level and lifecycle lookups, on_load, on_update,
/// on_unload) so the host can keep one watchdog account per call, and it says when the
/// host is stopping. While it is, the runtime records no mod error, runs no further
/// mod code in the current call and returns; `stop(Normal)` clears that latch, so the
/// host's orderly leave still runs every on_unload.
pub trait CallbackPort {
    /// Before a call of `owner`'s code.
    fn enter(&self, owner: &str);
    /// After that call returned or failed. `Some(failure)` when the host's watchdog
    /// ended the call: the call then fails with it even if the mod caught
    /// the host's error and returned normally.
    fn exit(&self) -> Option<String>;
    /// The host is stopping now (its count hook raises its stop at the next check).
    fn interrupted(&self) -> bool;
}

pub struct OwnerRegistration {
    pub state: Rc<OwnerState>,
    pub settings: Settings,
}
pub struct RuntimeSetup {
    /// Discovery's dependency-first candidate IDs, not frozen admission.
    pub plan: Vec<String>,
    /// Explicit host admission. No fallback/loader policy is inferred here.
    pub admitted: BTreeSet<String>,
    pub owners: BTreeMap<String, OwnerRegistration>,
    pub clock: Rc<dyn Monotonic>,
    pub cleanup: Rc<dyn Cleanup>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StopPhase {
    Normal,
    ProcessExit,
}
#[derive(Debug, Default)]
pub struct StopReport {
    pub failures: BTreeMap<String, String>,
    /// Late-exit cleanup was deliberately NOT attempted (reverse plan order).
    pub skipped: Vec<String>,
}
struct Callbacks {
    load: Option<RegistryKey>,
    update: Option<RegistryKey>,
    unload: Option<RegistryKey>,
}
pub(super) struct Entry {
    pub id: String,
    callbacks: Callbacks,
    pub interval: Duration,
    pub next: Duration,
    pub state: Rc<OwnerState>,
}

/// No Send, FFI, thread creation, source watcher or native job queue. All Lua
/// operations use the supplied production phase VM on its owning worker.
/// Watchdog accounting is a separate port; interruption/worker wiring is NOT
/// implemented by this pure layer and must be completed before player activation.
pub struct LifecycleRuntime {
    // Drop callback keys before the phase VM.
    pub(super) entries: Vec<Entry>,
    pub(super) owners: BTreeMap<String, OwnerRegistration>,
    pub(super) configurations: BTreeMap<String, AppliedConfiguration>,
    pub(super) errors: BTreeMap<String, String>,
    pub(super) clock: Rc<dyn Monotonic>,
    cleanup: Rc<dyn Cleanup>,
    next_refresh: Duration,
    observations: BTreeMap<String, super::Observation>,
    pub(super) stopping: bool,
    pub(super) retired: Vec<LifecycleRuntime>,
    exited: bool,
    port: Option<Rc<dyn CallbackPort>>,
    pub(super) interrupted: Cell<bool>,
    vm: LuaVm,
}
impl LifecycleRuntime {
    pub fn start(vm: LuaVm, registry: &Registry, setup: RuntimeSetup) -> mlua::Result<Self> {
        Self::start_with_port(vm, registry, setup, None)
    }
    /// [`Self::start`] with the worker's [`CallbackPort`]. An interruption during
    /// start stops activating further packages and still returns the runtime (check
    /// [`Self::interrupted`]): the host decides between its orderly and fast leave,
    /// and a fast leave must not drop the VM.
    pub fn start_with_port(
        vm: LuaVm,
        registry: &Registry,
        setup: RuntimeSetup,
        port: Option<Rc<dyn CallbackPort>>,
    ) -> mlua::Result<Self> {
        let mut seen = BTreeSet::new();
        // Validate composition before any top-level Lua runs. Admission is external.
        for id in &setup.plan {
            if !seen.insert(id) {
                return Err(mlua::Error::runtime("duplicate runtime candidate"));
            }
            let record = registry
                .get(id)
                .ok_or_else(|| mlua::Error::runtime(format!("unknown runtime candidate: {id}")))?;
            if setup.admitted.contains(id)
                && record
                    .parsed()
                    .manifest
                    .capabilities
                    .iter()
                    .any(|c| c.requires_runtime())
                && (!setup.owners.contains_key(id) || vm.environment(id).is_none())
            {
                return Err(mlua::Error::runtime(format!(
                    "admitted runtime owner is not registered: {id}"
                )));
            }
        }
        if setup.admitted.iter().any(|id| !seen.contains(id)) {
            return Err(mlua::Error::runtime(
                "admitted package is not a runtime candidate",
            ));
        }
        let mut runtime = Self {
            entries: Vec::new(),
            owners: setup.owners,
            configurations: registry
                .values()
                .map(|r| {
                    let m = r.parsed().manifest.clone();
                    (m.id.clone(), AppliedConfiguration::new(m))
                })
                .collect(),
            errors: BTreeMap::new(),
            next_refresh: setup.clock.now(),
            observations: BTreeMap::new(),
            clock: setup.clock,
            cleanup: setup.cleanup,
            stopping: false,
            retired: Vec::new(),
            exited: false,
            port,
            interrupted: Cell::new(false),
            vm,
        };
        // Both pins plan_runtime_mods.rs:49-78 seed blockers in candidate order
        // before entrypoints run. Retain blocked/disabled environments and their
        // top-level execution; this is activation policy, not host admission.
        for id in &setup.plan {
            let manifest = &registry
                .get(id)
                .expect("validated candidate")
                .parsed()
                .manifest;
            if !manifest.enabled || runtime.errors.contains_key(id) {
                continue;
            }
            let blocked = manifest.dependencies.iter().find_map(|dependency| {
                if dependency.id == "shroudforge-api" || dependency.optional.unwrap_or(false) {
                    return None;
                }
                if registry
                    .get(&dependency.id)
                    .is_none_or(|candidate| !candidate.parsed().manifest.enabled)
                {
                    Some(format!(
                        "required dependency is disabled or unavailable: {}",
                        dependency.id
                    ))
                } else {
                    runtime
                        .errors
                        .get(&dependency.id)
                        .map(|_| format!("required dependency is blocked: {}", dependency.id))
                }
            });
            if let Some(detail) = blocked {
                runtime
                    .errors
                    .insert(id.clone(), format!("dependency-blocked: {detail}"));
            }
        }
        for id in setup.plan {
            if runtime.interrupted.get() {
                break;
            }
            let manifest = &registry
                .get(&id)
                .expect("validated candidate")
                .parsed()
                .manifest;
            if !setup.admitted.contains(&id)
                || !manifest.capabilities.iter().any(|c| c.requires_runtime())
            {
                continue;
            }
            let value = match runtime.guarded(&id, || runtime.vm.entrypoint(&id)) {
                Ok(value) => value,
                Err(_) if runtime.interrupted.get() => break,
                Err(error) => {
                    runtime
                        .errors
                        .insert(id, format!("module initialization failed: {error}"));
                    continue;
                }
            };
            let table = match value {
                Value::Nil => None,
                Value::Table(table) => Some(table),
                _ => {
                    runtime
                        .errors
                        .insert(id, "entrypoint must return a lifecycle table or nil".into());
                    continue;
                }
            };
            // Lookups run __index metamethods: mod code, bracketed like any call.
            let interval = runtime.guarded(&id, || {
                table
                    .as_ref()
                    .map_or(Ok(None), |t| t.get::<Option<u64>>("update_interval_ms"))
            });
            if runtime.interrupted.get() {
                break;
            }
            let interval = match interval {
                Ok(None) => 50,
                Ok(Some(n)) if (8..=1000).contains(&n) => n,
                _ => {
                    runtime.errors.insert(
                        id,
                        "update_interval_ms must be an integer from 8 to 1000".into(),
                    );
                    continue;
                }
            };
            // Evaluate all three lookups, as the source tuple does (including __index).
            let lookups = runtime.guarded(&id, || {
                Ok((
                    callback(runtime.vm.lua(), table.as_ref(), "on_load"),
                    callback(runtime.vm.lua(), table.as_ref(), "on_update"),
                    callback(runtime.vm.lua(), table.as_ref(), "on_unload"),
                ))
            });
            if runtime.interrupted.get() {
                break;
            }
            // Without a port the bracket never fails; with one, only the watchdog's
            // lifecycle failure does (the stop was handled above).
            let callbacks = match lookups {
                Ok((Ok(load), Ok(update), Ok(unload))) => Callbacks {
                    load,
                    update,
                    unload,
                },
                _ => {
                    runtime.errors.insert(id, "invalid lifecycle".into());
                    continue;
                }
            };
            if manifest.enabled
                && let Some(dependency) = manifest.dependencies.iter().find(|d| {
                    !d.optional.unwrap_or(false)
                        && d.id != "shroudforge-api"
                        && runtime.errors.contains_key(&d.id)
                })
            {
                runtime.errors.insert(
                    id.clone(),
                    format!(
                        "dependency-blocked: required dependency failed to initialize: {}",
                        dependency.id
                    ),
                );
            }
            let enabled = manifest.enabled && !runtime.errors.contains_key(&id);
            let entry = Entry {
                id: id.clone(),
                callbacks,
                interval: Duration::from_millis(interval),
                next: runtime.clock.now(),
                state: runtime.owners[&id].state.clone(),
            };
            runtime.entries.push(entry);
            let index = runtime.entries.len() - 1;
            if enabled {
                // Registry lookup has an outer failure boundary during initial load.
                let outcome = runtime.activate(index)?;
                if runtime.interrupted.get() {
                    break;
                }
                if let Err(error) = outcome {
                    runtime
                        .errors
                        .insert(id, format!("on_load failed: {error}"));
                    if runtime.try_unload(index).is_ok() {
                        runtime.entries.pop();
                    }
                }
            }
        }
        // start_ingame_runtime.rs:127 (1.7.9), :129 (1.8): first status pass is due now,
        // after initial on_load work, not one second after construction began.
        runtime.next_refresh = runtime.clock.now();
        Ok(runtime)
    }
    pub fn vm(&self) -> &LuaVm {
        &self.vm
    }
    pub fn errors(&self) -> &BTreeMap<String, String> {
        &self.errors
    }
    pub fn owner(&self, id: &str) -> Option<&Rc<OwnerState>> {
        self.owners.get(id).map(|o| &o.state)
    }
    pub fn next_update(&self, id: &str) -> Option<Duration> {
        self.entries.iter().find(|e| e.id == id).map(|e| e.next)
    }
    /// Successfully activated/runnable owners, excluding cleanup-only scopes.
    /// Their OwnerState may still hold active permission until restoration succeeds.
    pub fn active_ids(&self) -> Vec<String> {
        self.entries
            .iter()
            .filter(|e| e.state.active() && !e.state.cleanup_pending())
            .map(|e| e.id.clone())
            .collect()
    }
    /// The worker's port reported a stop during the last call into mod code.
    pub fn interrupted(&self) -> bool {
        self.interrupted.get()
    }
    /// Runs `f` (a call into `owner`'s code) inside the port's bracket. Once the port
    /// reports a stop, no further mod code runs until `stop(Normal)`.
    pub(super) fn guarded<R>(
        &self,
        owner: &str,
        f: impl FnOnce() -> mlua::Result<R>,
    ) -> mlua::Result<R> {
        let Some(port) = &self.port else {
            return f();
        };
        if self.stop_in_force() {
            return Err(mlua::Error::runtime("the interpreter is stopping"));
        }
        // A panic out of `f` (a Rust binding) still closes the bracket.
        struct Exit<'a>(Option<&'a dyn CallbackPort>);
        impl Drop for Exit<'_> {
            fn drop(&mut self) {
                if let Some(port) = self.0.take() {
                    let _ = port.exit();
                }
            }
        }
        port.enter(owner);
        let mut bracket = Exit(Some(port.as_ref()));
        let result = f();
        let failure = bracket.0.take().and_then(|port| port.exit());
        if port.interrupted() {
            self.interrupted.set(true);
        }
        match (result, failure) {
            // Over the watchdog's limit: a lifecycle failure even when the mod caught
            // the host's error (or the whole overrun was one engine call) and returned.
            (Ok(_), Some(failure)) => Err(mlua::Error::runtime(failure)),
            (result, _) => result,
        }
    }
    /// The port reports a stop now, or one was latched: latched from here on (no mod code runs).
    fn stop_in_force(&self) -> bool {
        if !self.interrupted.get() && self.port.as_ref().is_some_and(|p| p.interrupted()) {
            self.interrupted.set(true);
        }
        self.interrupted.get()
    }
    pub fn quiescent(&self) -> bool {
        self.entries.iter().all(|e| !e.state.active()) && self.retired.iter().all(Self::quiescent)
    }
    /// Shared phase-VM memory, not a fabricated per-package allocation count/cap.
    pub fn used_memory(&self) -> usize {
        self.vm.lua().used_memory()
    }

    /// Marks the scope active and runs its on_load. The scope becomes active only when its on_load is about to
    /// run (or it has none): an activation the host's stop refused leaves it inactive, so the orderly leave owes it
    /// no on_unload (a restore after a reload's unload never unloads a scope twice).
    pub(super) fn activate(&self, index: usize) -> mlua::Result<mlua::Result<()>> {
        let entry = &self.entries[index];
        let function = entry
            .callbacks
            .load
            .as_ref()
            .map(|k| self.vm.lua().registry_value::<Function>(k))
            .transpose()?;
        Ok(match function {
            Some(f) => self.guarded(&entry.id, || {
                entry.state.active.set(true);
                f.call::<()>(())
            }),
            None if self.stop_in_force() => {
                Err(mlua::Error::runtime("the interpreter is stopping"))
            }
            None => {
                entry.state.active.set(true);
                Ok(())
            }
        })
    }
    pub(super) fn try_unload(&self, index: usize) -> mlua::Result<()> {
        let entry = &self.entries[index];
        entry.state.cleanup_pending.set(true);
        let callback = entry.callbacks.unload.as_ref().map_or(Ok(()), |key| {
            let function = self.vm.lua().registry_value::<Function>(key)?;
            self.guarded(&entry.id, || function.call::<()>(()))
        });
        if self.interrupted.get() {
            // Stopping: no restoration and no cleanup verdict; the host decides.
            return Err(mlua::Error::runtime("the interpreter is stopping"));
        }
        // Always attempt restore, even if on_unload errored. Callback error wins.
        let cleanup = self
            .cleanup
            .restore(&entry.id)
            .map_err(mlua::Error::runtime);
        if self.vm.context().version == SourceVersion::Sf180 {
            self.cleanup.release_input_reservations(&entry.id);
        }
        if let Err(error) = callback.and(cleanup) {
            *entry.state.cleanup_error.borrow_mut() = Some(error.to_string());
            return Err(error);
        }
        *entry.state.cleanup_error.borrow_mut() = None;
        entry.state.cleanup_pending.set(false);
        entry.state.active.set(false);
        Ok(())
    }
    /// Configuration observations are retained and applied on the next due 1s
    /// refresh BEFORE that turn's updates. The host owns source polling cadence
    /// (1.8's initial unobserved sample is explicit).
    /// `dt` is captured once by the host; earlier callbacks/waits cannot change it.
    pub fn turn(&mut self, dt: f64, observations: &[super::ConfigurationObservation]) {
        if self.exited || self.interrupted.get() {
            return;
        }
        if !self.stopping {
            for observation in observations {
                if self.configurations.contains_key(&observation.id) {
                    self.observations
                        .insert(observation.id.clone(), observation.observation.clone());
                }
            }
            // Both pins update_ingame_runtime.rs:17-45: a delayed refresh sets
            // the next deadline to completion+1s, with no replay/catch-up bursts.
            if self.clock.now() >= self.next_refresh {
                let latest: Vec<_> = self
                    .observations
                    .iter()
                    .map(|(id, observation)| super::ConfigurationObservation {
                        id: id.clone(),
                        observation: observation.clone(),
                    })
                    .collect();
                self.refresh_configuration(&latest);
                if self.interrupted.get() {
                    return;
                }
                self.next_refresh = self.clock.now().saturating_add(Duration::from_secs(1));
            }
        }
        for retired in &mut self.retired {
            retired.turn(dt, &[]);
        }
        self.retired.retain(|r| !r.quiescent());
        for index in 0..self.entries.len() {
            let entry = &self.entries[index];
            if !entry.state.active() {
                continue;
            }
            if entry.state.cleanup_pending() {
                if self.clock.now() < entry.next {
                    continue;
                }
                let _ = self.try_unload(index);
                if self.interrupted.get() {
                    return;
                }
                self.entries[index].next = self
                    .clock
                    .now()
                    .saturating_add(self.entries[index].interval);
                continue;
            }
            if self.stopping || self.clock.now() < entry.next {
                continue;
            }
            let Some(key) = entry.callbacks.update.as_ref() else {
                continue;
            };
            let result = self
                .vm
                .lua()
                .registry_value::<Function>(key)
                .and_then(|f| self.guarded(&self.entries[index].id, || f.call::<()>(dt)));
            if self.interrupted.get() {
                // The host's stop, not a mod failure: no error, no unload, no
                // deadline change; the host leaves now.
                return;
            }
            let entry = &mut self.entries[index];
            entry.next = advance_deadline(entry.next, entry.interval, self.clock.now());
            if let Err(error) = result {
                self.errors
                    .insert(entry.id.clone(), format!("on_update failed: {error}"));
                let _ = self.try_unload(index);
            }
        }
    }
    /// Normal shutdown attempts each active scope once, in reverse order. A pending
    /// owner remains permitted and retains its keys; future turns only retry cleanup.
    /// Late exit has NO explicitly exit-safe script port, so skips scripts AND game
    /// restoration. Repeated normal stop after late exit cannot re-enter engine code.
    pub fn stop(&mut self, phase: StopPhase) -> StopReport {
        let mut report = StopReport::default();
        self.stopping = true;
        // The host's leave is the stop it asked for: on_unload runs again from here.
        self.interrupted.set(false);
        if self.exited {
            return report;
        }
        for retired in &mut self.retired {
            let retired_report = retired.stop(phase);
            report.failures.extend(retired_report.failures);
            report.skipped.extend(retired_report.skipped);
        }
        if phase == StopPhase::ProcessExit {
            self.exited = true;
            report.skipped.extend(
                self.entries
                    .iter()
                    .rev()
                    .filter(|e| e.state.active())
                    .map(|e| e.id.clone()),
            );
            return report;
        }
        for index in (0..self.entries.len()).rev() {
            if !self.entries[index].state.active() {
                continue;
            }
            if let Err(error) = self.try_unload(index) {
                if self.interrupted.get() {
                    // Cancelled during the leave: the rest is skipped, unrecorded.
                    report.skipped.extend(
                        self.entries[..=index]
                            .iter()
                            .rev()
                            .filter(|e| e.state.active())
                            .map(|e| e.id.clone()),
                    );
                    break;
                }
                report
                    .failures
                    .insert(self.entries[index].id.clone(), error.to_string());
            }
        }
        report
    }
    pub(super) fn configured_manifest(&self, id: &str) -> Option<&Manifest> {
        self.configurations.get(id).map(|c| &c.manifest)
    }
}
fn callback(
    lua: &mlua::Lua,
    table: Option<&Table>,
    name: &str,
) -> mlua::Result<Option<RegistryKey>> {
    match table.map_or(Ok(Value::Nil), |t| t.get::<Value>(name))? {
        Value::Nil => Ok(None),
        Value::Function(f) => Ok(Some(lua.create_registry_value(f)?)),
        _ => Err(mlua::Error::runtime(format!("{name} must be a function"))),
    }
}
