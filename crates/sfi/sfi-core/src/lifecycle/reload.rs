// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Both pins, modules/mod-runtime/ingame/: process_runtime_reload_request.rs:10-46,
// unload_runtime_packages.rs:5-25, reload_runtime_packages.rs:6-95,
// restore_runtime_packages.rs:4-22. No source paths, watcher or physical writer.
use super::{LifecycleRuntime, StopPhase};
use serde_json::{Value, json};

/// Last-request suppression is host-owned and survives runtime replacement.
#[derive(Default)]
pub struct ReloadProtocol {
    last: Option<String>,
}
impl ReloadProtocol {
    pub fn last_request(&self) -> Option<&str> {
        self.last.as_deref()
    }
    /// Parse a host-read value; consume the token BEFORE dispatch. Publication is
    /// attempted once, best effort; failure does not retry dispatch or publication.
    pub fn process(
        &mut self,
        request: &Value,
        dispatch: impl FnOnce(&str, &[String]) -> Result<(), String>,
        publish: impl FnOnce(&Value) -> Result<(), String>,
    ) -> bool {
        let Some(id) = request.get("requestId").and_then(Value::as_str) else {
            return false;
        };
        if self.last.as_deref() == Some(id) {
            return false;
        }
        self.last = Some(id.into());
        let ids: Vec<_> = request
            .get("modIds")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter_map(|v| v.as_str().map(str::to_owned))
            .collect();
        let operation = request
            .get("operation")
            .and_then(Value::as_str)
            .unwrap_or("reload");
        let result = match operation {
            "reload" | "unload" => dispatch(operation, &ids),
            other => Err(format!("unknown runtime mod operation: {other}")),
        };
        let value = match result {
            Ok(()) => {
                json!({"requestId":id, "status":if operation == "unload" {"unloaded"} else {"reloaded"}, "message":"Runtime mod packages updated"})
            }
            Err(error) => json!({"requestId":id,"status":"error","message":error}),
        };
        let _ = publish(&value);
        true
    }
}
impl LifecycleRuntime {
    /// Requested IDs are filters, not a dependency closure. Failure reactivates
    /// ONLY scopes already unloaded, in forward plan order, on the same closures.
    pub fn unload(&mut self, ids: &[String]) -> Result<(), String> {
        if self.stopping {
            return Err("interpreter is stopping".into());
        }
        let mut unloaded = Vec::new();
        for index in (0..self.entries.len()).rev() {
            let entry = &self.entries[index];
            if !entry.state.active() || !ids.contains(&entry.id) {
                continue;
            }
            if let Err(error) = self.try_unload(index) {
                let id = self.entries[index].id.clone();
                self.restore_previous(&unloaded);
                return Err(format!("mod '{id}' on_unload failed: {error}"));
            }
            unloaded.push(self.entries[index].id.clone());
        }
        Ok(())
    }
    /// Replace the WHOLE runtime set. `build` performs host rediscovery/admission
    /// and constructs a new registered VM; requested IDs only validate its outcome.
    /// The host must carry process clock/epochs/admission and must not fall back to
    /// another backend after effects begin. Lua/DLL/file/archive effects are NOT a
    /// transaction; failed replacement reruns prior on_load, retaining old locals.
    pub fn reload(
        &mut self,
        requested: &[String],
        build: impl FnOnce() -> Result<Self, String>,
    ) -> Result<(), String> {
        if self.stopping {
            return Err("interpreter is stopping".into());
        }
        let previous: Vec<_> = self
            .entries
            .iter()
            .filter(|e| e.state.active() && !e.state.cleanup_pending())
            .map(|e| e.id.clone())
            .collect();
        for index in (0..self.entries.len()).rev() {
            if !self.entries[index].state.active() {
                continue;
            }
            if let Err(error) = self.try_unload(index) {
                let id = self.entries[index].id.clone();
                self.restore_previous(&previous);
                return Err(format!(
                    "mod '{id}' cleanup prevented package refresh: {error}"
                ));
            }
        }
        let mut replacement = match build() {
            Ok(runtime) => runtime,
            Err(error) => {
                self.restore_previous(&previous);
                return Err(error);
            }
        };
        let invalid = requested.iter().find_map(|id| {
            let manifest = replacement.configured_manifest(id)?;
            if manifest.enabled
                && manifest.capabilities.iter().any(|c| c.requires_runtime())
                && !replacement.active_ids().contains(id)
            {
                let reason = replacement
                    .errors
                    .get(id)
                    .map(String::as_str)
                    .unwrap_or("updated mod did not become active");
                Some(format!(
                    "updated runtime mod '{id}' failed to activate: {reason}"
                ))
            } else {
                None
            }
        });
        if let Some(error) = invalid {
            replacement.stop(StopPhase::Normal);
            // Keep callbacks/VM alive if caller-thread restore refused busy.
            if !replacement.quiescent() {
                self.retired.push(replacement);
            }
            self.restore_previous(&previous);
            return Err(error);
        }
        // Preserve any older rejected, cleanup-pending VM until it is quiescent.
        replacement.retired.append(&mut self.retired);
        *self = replacement;
        Ok(())
    }
    fn restore_previous(&mut self, ids: &[String]) {
        for index in 0..self.entries.len() {
            let entry = &self.entries[index];
            if !ids.contains(&entry.id) || entry.state.cleanup_pending() {
                continue;
            }
            // Even an old scope that was not yet unloaded receives on_load again.
            if let Err(error) = self.activate(index).and_then(|r| r) {
                if self.interrupted.get() {
                    // The host's stop: no error and no cleanup. A scope whose on_load did not start stays
                    // inactive (its on_unload already ran), so the orderly leave never unloads it twice.
                    break;
                }
                self.errors.insert(
                    self.entries[index].id.clone(),
                    format!("Could not restore runtime mod after refresh rollback: {error}"),
                );
                let _ = self.try_unload(index);
            }
        }
    }
}
