// SPDX-License-Identifier: MIT
// the interpreter's UI bridge, ShroudForge's ui.on_action and its dispatch.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (1.7.9; the same in 1.8.0): modules/mod-runtime/lua/shroudforge/v1/create_mod_api.rs:191-206 (the ui table, on_action),
// :379-381 (action_registry_key), dispatch_mod_ui_actions.rs:21-53 (resolve and invoke) and
// ingame/update_ingame_runtime.rs:47 (the dispatch before the on_update callbacks).
//! `shroudforge.ui.on_action(action, callback)` and the Lua side of the action drain.
//!
//! `on_action` stores the callback in the VM's named registry under ShroudForge's dotted key
//! `shroudforge.ui.action.<modId>.<action>`, verbatim: one VM holds every package, so `foo.bar`/`baz` and
//! `foo`/`bar.baz` share one key and the later registration wins, whichever package made it. A request is checked
//! against its requested owner only (active at the request's epoch), then the key is looked up; a callback registered
//! by another owner, even an inactive one, still answers it, as in ShroudForge.
//!
//! [`dispatch`] runs one drain turn ([`transport::drain`]) on the worker at the start of the turn, before the
//! on_update dispatch. Each callback runs inside the lifecycle port's bracket (the watchdog's account, the stop), so a
//! callback that runs too long is stopped by the watchdog; that call fails and is logged, and the package stays active
//! (ShroudForge never fails a package for a UI action). While the host is stopping no callback runs and requests
//! stay in the folder.

use super::transport::{self, ActionTarget, DrainReport, Invoke, Transport, valid_identifier};
use mlua::{Function, Lua, Table};
use sfi_core::lifecycle::CallbackPort;

/// ShroudForge's registry key of an action callback (create_mod_api.rs:379-381).
pub fn action_registry_key(mod_id: &str, action: &str) -> String {
    format!("shroudforge.ui.action.{mod_id}.{action}")
}

/// `ui.on_action` of package `mod_id`, verbatim (create_mod_api.rs:193-201): an invalid mod or action id raises
/// ShroudForge's `invalid ui action`; a callback replaces the one under the same key; it returns nothing.
pub fn install(lua: &Lua, ui: &Table, mod_id: &str) -> mlua::Result<()> {
    let ui_mod_id = mod_id.to_owned();
    ui.raw_set(
        "on_action",
        lua.create_function(move |lua, (action, callback): (String, Function)| {
            if !valid_identifier(&ui_mod_id) || !valid_identifier(&action) {
                return Err(mlua::Error::runtime("invalid ui action"));
            }
            lua.set_named_registry_value(&action_registry_key(&ui_mod_id, &action), callback)
        })?,
    )
}

/// The drain's owners and callbacks in a VM: the active packages, all at the VM's current owner epoch.
pub struct LuaTarget<'a> {
    pub lua: &'a Lua,
    pub active: &'a [String],
    pub epoch: u64,
    /// The lifecycle port that brackets every mod call (none in tests that drive a bare VM).
    pub port: Option<&'a dyn CallbackPort>,
}
impl ActionTarget for LuaTarget<'_> {
    fn epoch(&self, mod_id: &str) -> Option<u64> {
        self.active
            .iter()
            .any(|id| id == mod_id)
            .then_some(self.epoch)
    }
    fn invoke(&mut self, mod_id: &str, action: &str, value: Option<&str>) -> Invoke {
        if self.port.is_some_and(|port| port.interrupted()) {
            // stopping: no mod code runs now, and the request stays
            return Invoke::Unregistered;
        }
        let Ok(callback) = self
            .lua
            .named_registry_value::<Function>(&action_registry_key(mod_id, action))
        else {
            return Invoke::Unregistered;
        };
        struct Exit<'a>(Option<&'a dyn CallbackPort>);
        impl Drop for Exit<'_> {
            fn drop(&mut self) {
                if let Some(port) = self.0.take() {
                    let _ = port.exit();
                }
            }
        }
        if let Some(port) = self.port {
            port.enter(mod_id);
        }
        let mut bracket = Exit(self.port);
        let result = match value {
            Some(value) => callback.call::<()>(value),
            None => callback.call::<()>(()),
        };
        let failure = bracket.0.take().and_then(|port| port.exit());
        Invoke::Called(match (result, failure) {
            (_, Some(failure)) => Err(failure),
            (result, None) => result.map_err(|error| error.to_string()),
        })
    }
}

/// One drain turn of this launch's actions in `lua` (the runtime's VM): `active` are the packages active now; a
/// callback's error goes to `log`, not retried: ShroudForge's `ui action failed` line (target `shroudforge::mod`, fields
/// mod_id and action) in Kiln's layout for ShroudForge's lines, `<target>: <text> (<field>=<value>, ...)`. A
/// watchdog stop of a callback is only that line too: ShroudForge has no watchdog and never marks a package failed for
/// a UI action, so the package stays active (unlike an on_update failure).
pub fn dispatch(
    lua: &Lua,
    active: &[String],
    transport: &Transport,
    port: Option<&dyn CallbackPort>,
    log: impl Fn(&str),
) -> DrainReport {
    let mut target = LuaTarget {
        lua,
        active,
        epoch: transport::owner_epoch(lua),
        port,
    };
    let report = transport.drain(&mut target);
    for (mod_id, action, error) in report.failures() {
        log(&format!(
            "shroudforge::mod: ui action failed: {error} (mod_id={mod_id}, action={action})"
        ));
    }
    report
}
