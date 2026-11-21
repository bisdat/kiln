// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (identical in both pins): modules/mod-runtime/lua/ingame/overrides/
// track_runtime_overrides.rs:1-72 (create_scoped: the per-mod factory and tracker registry; cleanup),
// track_runtime_overrides.lua (tracker.lua beside this file, with its own notes).
//! ShroudForge's override tracker (`runtime.overrides.new`) and its automatic
//! mod-scope cleanup.
//!
//! The tracker is ShroudForge's own Lua (`tracker.lua` beside this file), loaded once per mod in the
//! runtime-create's VM, so `owned / blocked / touched / details`, the counters, `fault` and
//! `session` stay the plain mutable Lua tables and fields mods see in ShroudForge, and
//! `active_keys` is per mod as there. The trackers of one mod are kept in a named registry
//! table for that mod id, so rebuilding the mod's environment keeps them (and its ownership).
//!
//! What Kiln adds (tracker.lua's "Kiln:" lines): an entry remembers the nonzero live session it
//! was bound in, and `restore` never writes into another nonzero session (non-shared: dropped
//! without a write; shared: waits, kept). Restores only write while the read value is still the
//! last applied one (ShroudForge's own rule); each target's `write` is its family's CAS.
//!
//! [`OverrideTrackers`] is the lifecycle's [`Cleanup`] port: on a mod's unload (after its
//! `on_unload`, even a failing one) it runs, on the worker, `begin_update` + `restore_all` on each of
//! that mod's trackers through their own target closures and answers ShroudForge's texts. A pending
//! cleanup keeps the mod active (its permissions and claims) and is retried by later turns; the fast
//! leave never calls it.
//!
//! The live session is `kiln.world.game_world_session` of [`session_world_kind`]
//! (the server world on the server side, the client world otherwise), read once
//! per tracker operation that needs it.

use crate::runtime::install::Family;
use kiln_abi::{KILN_WORLD_CLIENT, KILN_WORLD_SERVER};
use mlua::{Function, Lua, Table, WeakLua};
use sfi_core::host::Side;
use sfi_core::lifecycle::Cleanup;
use std::cell::RefCell;
use std::rc::Rc;

/// ShroudForge's tracker, with Kiln's session rule.
const TRACKER: &str = include_str!("tracker.lua");

/// The mod's tracker list in the VM's registry. A mod id may contain dots but never `/`, so no
/// mod's key is another mod's key plus a suffix (ShroudForge's `.{id}` / `.{id}.factory` keys
/// collide for ids `x` and `x.factory`).
fn registry_key(owner: &str) -> String {
    format!("kiln.sfi.runtime.overrides/{owner}/trackers")
}

/// The mod's cached `new` in the VM's registry.
fn factory_key(owner: &str) -> String {
    format!("kiln.sfi.runtime.overrides/{owner}/factory")
}

/// The world whose `game_world_session` is ShroudForge's `world_session_id` for a runtime of
/// `side` (the ruling every family reuses).
pub fn session_world_kind(side: Side) -> u32 {
    match side {
        Side::Server => KILN_WORLD_SERVER as u32,
        Side::Client => KILN_WORLD_CLIENT as u32,
    }
}

/// The override trackers of one runtime-create: `runtime.overrides.new` per package, and the
/// lifecycle's cleanup port over them.
#[derive(Default)]
pub struct OverrideTrackers {
    /// The runtime-create's VM, bound at the first install (the cleanup runs in it).
    lua: RefCell<Option<WeakLua>>,
}

impl OverrideTrackers {
    pub fn new() -> Rc<Self> {
        Rc::new(Self::default())
    }

    /// The runtime family that installs `runtime.overrides.new` for each package, its live
    /// session read from the runtime-create's facts on world `world_kind`.
    pub fn family(self: &Rc<Self>, world_kind: u32) -> Family {
        let trackers = self.clone();
        Family::new(
            "overrides.tracker",
            &["runtime.overrides.new"],
            move |lua, _env, package, composed, cx| {
                let Some(runtime) = &composed.runtime else {
                    return Ok(());
                };
                let overrides: Table = runtime.raw_get("overrides")?;
                let facts = cx.facts().clone();
                trackers.install(
                    lua,
                    &overrides,
                    package.id(),
                    Rc::new(move || facts.world_session(world_kind)),
                )
            },
        )
    }

    /// Sets `overrides.new` for mod `owner`: the mod's cached factory (ShroudForge's
    /// `create_scoped`), whose trackers read the live session through `session`. A rebuilt
    /// environment of the same mod gets the same factory and tracker registry.
    pub fn install(
        &self,
        lua: &Lua,
        overrides: &Table,
        owner: &str,
        session: Rc<dyn Fn() -> u64>,
    ) -> mlua::Result<()> {
        {
            let mut bound = self.lua.borrow_mut();
            let weak = lua.weak();
            match bound.as_ref() {
                Some(existing) if existing.try_upgrade().is_some() && *existing != weak => {
                    return Err(mlua::Error::runtime(
                        "override trackers are bound to another runtime VM",
                    ));
                }
                Some(existing) if *existing == weak => {}
                _ => *bound = Some(weak),
            }
        }
        overrides.raw_set("new", factory_new(lua, owner, session)?)
    }

    /// ShroudForge's `cleanup` for mod `owner`: each tracker in creation order, `begin_update`
    /// then `restore_all`; the first fault answers `Runtime API cleanup requires inspection:
    /// <fault>`, the first tracker still owning something its summary under `Runtime API cleanup
    /// is pending`. Nothing to do before a VM is bound or after it is gone.
    pub fn cleanup(&self, owner: &str) -> Result<(), String> {
        let lua = self.lua.borrow().as_ref().and_then(WeakLua::try_upgrade);
        let Some(lua) = lua else {
            return Ok(());
        };
        let trackers: Option<Table> = lua
            .named_registry_value(&registry_key(owner))
            .map_err(|error| error.to_string())?;
        let Some(trackers) = trackers else {
            return Ok(());
        };
        for tracker in trackers.sequence_values::<Table>() {
            let tracker = tracker.map_err(|error| error.to_string())?;
            let call = |name: &str| -> mlua::Result<()> {
                tracker.raw_get::<Function>(name)?.call(tracker.clone())
            };
            call("begin_update").map_err(|error| error.to_string())?;
            call("restore_all").map_err(|error| error.to_string())?;
            if let Some(fault) = tracker
                .raw_get::<Option<String>>("fault")
                .map_err(|error| error.to_string())?
            {
                return Err(format!("Runtime API cleanup requires inspection: {fault}"));
            }
            let owned: bool = tracker
                .raw_get::<Function>("has_ownership")
                .and_then(|function| function.call(tracker.clone()))
                .map_err(|error| error.to_string())?;
            if owned {
                return Err(tracker
                    .raw_get::<Function>("summary")
                    .and_then(|function| {
                        function.call((tracker.clone(), "Runtime API cleanup is pending"))
                    })
                    .unwrap_or_else(|error| error.to_string()));
            }
        }
        Ok(())
    }
}

impl Cleanup for OverrideTrackers {
    fn restore(&self, owner: &str) -> Result<(), String> {
        self.cleanup(owner)
    }
    /// 1.8 only; nothing 1.8-only is switched on.
    fn release_input_reservations(&self, _: &str) {}
}

/// The mod's `new` (cached in the VM's registry): a new tracker on `session`, appended to the
/// mod's tracker registry. Extra arguments are ignored, as ShroudForge's unit closure does.
fn factory_new(lua: &Lua, owner: &str, session: Rc<dyn Fn() -> u64>) -> mlua::Result<Function> {
    let key = registry_key(owner);
    let factory_key = factory_key(owner);
    if let Some(new) = lua.named_registry_value::<Option<Function>>(&factory_key)? {
        return Ok(new);
    }
    let factory: Table = lua.load(TRACKER).set_name("runtime.overrides").eval()?;
    let new: Function = factory.raw_get("new")?;
    let trackers = lua.create_table()?;
    lua.set_named_registry_value(&key, trackers.clone())?;
    let get_session = lua.create_function(move |_, ()| Ok(session()))?;
    let wrapped = lua.create_function(move |_, _: mlua::MultiValue| {
        let tracker: Table = new.call(get_session.clone())?;
        trackers.raw_set(trackers.raw_len() + 1, tracker.clone())?;
        Ok(tracker)
    })?;
    lua.set_named_registry_value(&factory_key, wrapped.clone())?;
    Ok(wrapped)
}
