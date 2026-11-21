// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// modules/mod-runtime/ingame/refresh_runtime_configuration.rs:4-359 (1.8),
// :4-332 (1.7.9); watch_package_revisions.rs:41-98 (1.8 only).
// 1.8 normalizes reservation metadata and samples off-thread; 1.7.9 polls directly.
// Host supplies observations; this layer creates no watcher and does no source IO.
use super::LifecycleRuntime;
use crate::{
    host::SourceVersion,
    package::{Manifest, manifest::Capability, revisions},
    settings::Settings,
};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Debug)]
pub enum Observation {
    /// 1.8's first background sample has not arrived. Retain everything.
    Unobserved,
    ReadError(String),
    /// Host-observed ORIGINAL source manifest revision plus an effective discovered model.
    /// This revision is neither the config editing token nor loaded identity.
    Manifest {
        revision: String,
        manifest: Box<Manifest>,
    },
}
#[derive(Clone, Debug)]
pub struct ConfigurationObservation {
    pub id: String,
    pub observation: Observation,
}
impl ConfigurationObservation {
    pub fn manifest(id: &str, revision: &str, manifest: Manifest) -> Self {
        Self {
            id: id.into(),
            observation: Observation::Manifest {
                revision: revision.into(),
                manifest: Box::new(manifest),
            },
        }
    }
}
pub(super) struct AppliedConfiguration {
    pub manifest: Manifest,
    pub startup_values: Value,
    pub observed: Option<String>,
    pub loaded_revision: String,
}
impl AppliedConfiguration {
    pub fn new(manifest: Manifest) -> Self {
        Self {
            startup_values: manifest.setting_values.clone(),
            observed: None,
            loaded_revision: revisions::loaded_revision(&manifest),
            manifest,
        }
    }
}
/// Source metadata equivalence; desired defaults and (1.8) reservation padlocks
/// don't require code reload. All other changes remain deferred to explicit reload.
pub fn settings_only_manifest_change(
    desired: &Manifest,
    applied: &Manifest,
    version: SourceVersion,
) -> bool {
    let mut metadata = serde_json::to_value(desired).expect("manifest is serializable");
    let applied = serde_json::to_value(applied).expect("manifest is serializable");
    metadata["settingValues"] = applied["settingValues"].clone();
    metadata["enabled"] = applied["enabled"].clone();
    let applied_settings = applied["settings"]
        .as_array()
        .expect("manifest settings are an array");
    for setting in metadata["settings"]
        .as_array_mut()
        .expect("manifest settings are an array")
    {
        let Some(previous) = applied_settings
            .iter()
            .find(|p| p.get("key") == setting.get("key"))
        else {
            continue;
        };
        if let Some(default) = previous.get("default") {
            setting["default"] = default.clone();
        }
        if version == SourceVersion::Sf180 {
            match previous.get("reserved") {
                Some(value) => setting["reserved"] = value.clone(),
                None => {
                    setting
                        .as_object_mut()
                        .expect("serialized setting")
                        .remove("reserved");
                }
            }
        }
    }
    metadata == applied
}
impl LifecycleRuntime {
    /// Loaded identity, separate from original-source observations and config tokens.
    pub fn loaded_revision(&self, id: &str) -> Option<&str> {
        self.configurations
            .get(id)
            .map(|c| c.loaded_revision.as_str())
    }
    pub fn observed_revision(&self, id: &str) -> Option<&str> {
        self.configurations
            .get(id)
            .and_then(|c| c.observed.as_deref())
    }
    pub fn refresh_configuration(&mut self, observations: &[ConfigurationObservation]) {
        if self.stopping {
            return;
        }
        // Source snapshots are captured BEFORE any transitions in this refresh.
        let configured: BTreeMap<_, _> = self
            .configurations
            .iter()
            .map(|(id, c)| (id.clone(), c.manifest.enabled))
            .collect();
        // Both pins refresh_runtime_configuration.rs use item.active, INCLUDING
        // cleanup-pending owners, for dependency transitions. This is deliberately
        // not active_ids(), whose successful-activation membership excludes them.
        let active: BTreeSet<_> = self
            .entries
            .iter()
            .filter(|entry| entry.state.active())
            .map(|entry| entry.id.clone())
            .collect();
        let mut dependents = BTreeMap::<String, Vec<String>>::new();
        for (id, c) in &self.configurations {
            if !active.contains(id) {
                continue;
            }
            for d in &c.manifest.dependencies {
                if !d.optional.unwrap_or(false) {
                    dependents.entry(d.id.clone()).or_default().push(id.clone());
                }
            }
        }
        // The source registry order is sorted, independent of producer sample order.
        let observations: BTreeMap<_, _> = observations
            .iter()
            .map(|o| (o.id.as_str(), &o.observation))
            .collect();
        let ids: Vec<_> = self.configurations.keys().cloned().collect();
        for id in ids {
            let Some(observation) = observations.get(id.as_str()) else {
                continue;
            };
            let (revision, manifest) = match observation {
                Observation::Unobserved => continue,
                Observation::ReadError(error) => {
                    self.configurations.get_mut(&id).unwrap().observed =
                        Some(format!("read-error:{error}"));
                    continue;
                }
                Observation::Manifest { revision, manifest } => (revision, manifest.as_ref()),
            };
            let config = &self.configurations[&id];
            if config.observed.as_ref() == Some(revision) {
                continue;
            }
            self.configurations.get_mut(&id).unwrap().observed = Some(revision.clone());
            if manifest.id != id {
                continue;
            }
            let applied = self.configurations[&id].manifest.clone();
            let desired = serde_json::to_value(manifest).expect("manifest is serializable");
            if desired == serde_json::to_value(&applied).expect("manifest is serializable") {
                if active.contains(&id)
                    && self
                        .errors
                        .get(&id)
                        .is_some_and(|e| e.starts_with("dependency-blocked:"))
                {
                    self.errors.remove(&id);
                }
                continue;
            }
            if !settings_only_manifest_change(manifest, &applied, self.vm().context().version) {
                continue;
            }
            let runtime = manifest.capabilities.iter().any(|c| c.requires_runtime());
            let live = runtime && !manifest.capabilities.contains(&Capability::Patch);
            if live && manifest.enabled != applied.enabled {
                let blocked = if manifest.enabled {
                    manifest
                        .dependencies
                        .iter()
                        .find(|d| {
                            !d.optional.unwrap_or(false)
                                && (configured.get(&d.id) == Some(&false)
                                    || self
                                        .entries
                                        .iter()
                                        .any(|e| e.id == d.id && !e.state.active()))
                        })
                        .map(|d| format!("required dependency is not active: {}", d.id))
                } else {
                    dependents.get(&id).map(|ids| {
                        format!(
                            "cannot disable this dependency while required by active mod(s): {}",
                            ids.join(", ")
                        )
                    })
                };
                if let Some(error) = blocked {
                    self.errors
                        .insert(id.clone(), format!("dependency-blocked: {error}"));
                } else if let Some(index) = self.entries.iter().position(|e| e.id == id) {
                    let result = if manifest.enabled && self.entries[index].state.cleanup_pending()
                    {
                        Err(mlua::Error::runtime(
                            "Previous runtime mod cleanup is still pending",
                        ))
                    } else if manifest.enabled {
                        self.activate(index).and_then(|r| r)
                    } else if self.entries[index].state.active() {
                        self.try_unload(index)
                    } else {
                        Ok(())
                    };
                    if self.interrupted.get() {
                        // The worker host is stopping: not a configuration verdict.
                        return;
                    }
                    match result {
                        Ok(()) => {
                            self.configurations.get_mut(&id).unwrap().manifest.enabled =
                                manifest.enabled;
                            self.errors.remove(&id);
                        }
                        Err(error) => {
                            if !manifest.enabled {
                                self.configurations.get_mut(&id).unwrap().observed = None;
                            }
                            if manifest.enabled {
                                let _ = self.try_unload(index);
                            }
                            self.errors
                                .insert(id.clone(), format!("live activation failed: {error}"));
                        }
                    }
                } else {
                    self.errors.insert(id.clone(), "restart-required: Runtime module was not present in the startup plan. Restart the game to activate it.".into());
                }
            }
            if manifest.setting_values != applied.setting_values
                && let Some(owner) = self.owners.get(&id)
            {
                let baseline = self.configurations[&id].startup_values.clone();
                if let Some(values) = Settings::values_to_apply_live(manifest, &baseline)
                    && values != applied.setting_values
                {
                    match owner
                        .settings
                        .apply_live(self.vm().lua(), manifest, &baseline)
                    {
                        Ok(true) => {
                            let c = self.configurations.get_mut(&id).unwrap();
                            c.manifest.setting_values = values;
                            c.manifest.settings = manifest.settings.clone();
                        }
                        Ok(false) => {}
                        Err(error) => {
                            self.errors.insert(
                                id.clone(),
                                format!("live settings update failed: {error}"),
                            );
                        }
                    }
                }
            }
            let c = self.configurations.get_mut(&id).unwrap();
            // Input reservation delivery is the input feed's. Retain metadata here, but do not
            // advertise delivery as implemented by this pure state transition.
            c.manifest.settings = manifest.settings.clone();
            if c.manifest.enabled == manifest.enabled
                && c.manifest.setting_values == manifest.setting_values
            {
                let mut loaded = manifest.clone();
                loaded.enabled = c.manifest.enabled;
                loaded.setting_values = c.manifest.setting_values.clone();
                c.loaded_revision = revisions::loaded_revision(&loaded);
            }
        }
    }
}
