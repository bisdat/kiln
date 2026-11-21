// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (both pins): modules/modloader/plan_mod_load_order.rs:18-47,150-335;
// check_mod_conflicts.rs:17-43; modules/mod-runtime/lib.rs API_VERSION = 1.0.0.
//! Pure candidate ordering. This is NOT Kiln's frozen whole-package admission.
use super::{discover::Registry, profiles::CompatibilityRules};
use serde::Serialize;
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlanIssue {
    pub mod_id: String,
    pub code: String,
    pub detail: String,
}
impl PlanIssue {
    pub fn new(id: &str, detail: String) -> Self {
        let lower = detail.to_ascii_lowercase();
        let code = if lower.contains("missing ") {
            "missing-dependency"
        } else if lower.contains("disabled dependency") {
            "dependency-disabled"
        } else if lower.contains("incompatible") {
            "dependency-version"
        } else if lower.contains("dependency cycle") {
            "dependency-cycle"
        } else if lower.contains("wrong process target") {
            "wrong-target"
        } else {
            "conflict-or-plan-error"
        };
        Self {
            mod_id: id.into(),
            code: code.into(),
            detail,
        }
    }
}
impl std::fmt::Display for PlanIssue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.mod_id, self.detail)
    }
}
#[derive(Clone, Debug, Default)]
pub struct Plan {
    pub ids: Vec<String>,
    pub issues: Vec<PlanIssue>,
}
impl Plan {
    pub fn compatibility_error(error: String) -> Self {
        Self {
            ids: Vec::new(),
            issues: vec![PlanIssue {
                mod_id: "__loader__".into(),
                code: "compatibility-rules".into(),
                detail: format!("compatibility rules: {error}"),
            }],
        }
    }
}
pub fn startup_plan(registry: &Registry, server: bool, rules: Option<&CompatibilityRules>) -> Plan {
    plan(registry, server, rules, false)
}
pub fn runtime_candidates(
    registry: &Registry,
    server: bool,
    rules: Option<&CompatibilityRules>,
) -> Plan {
    let mut plan = plan(registry, server, rules, true);
    let mut runtime: BTreeSet<String> = plan
        .ids
        .iter()
        .filter(|id| {
            registry.get(id).is_some_and(|p| {
                p.parsed()
                    .manifest
                    .capabilities
                    .iter()
                    .any(|c| c.requires_runtime())
            })
        })
        .cloned()
        .collect();
    loop {
        let before = runtime.len();
        for id in &plan.ids {
            if !runtime.contains(id) {
                continue;
            }
            for dep in &registry
                .get(id)
                .expect("plan ID belongs to registry")
                .parsed()
                .manifest
                .dependencies
            {
                if plan.ids.contains(&dep.id) {
                    runtime.insert(dep.id.clone());
                }
            }
        }
        if runtime.len() == before {
            break;
        }
    }
    plan.ids.retain(|id| runtime.contains(id));
    plan
}
fn plan(
    registry: &Registry,
    server: bool,
    rules: Option<&CompatibilityRules>,
    include_disabled: bool,
) -> Plan {
    let mut blocked = BTreeMap::new();
    if let Some(rules) = rules {
        for rule in &rules.conflicts {
            let matches = |c: &super::profiles::Constraint| {
                registry.get(&c.id).is_some_and(|p| {
                    p.parsed().manifest.enabled && c.version.matches(&p.parsed().manifest.version)
                })
            };
            if matches(&rule.first) && matches(&rule.second) {
                blocked.insert(rule.first.id.clone(), rule.reason.clone());
                blocked.insert(rule.second.id.clone(), rule.reason.clone());
            }
        }
    }
    struct Visitor<'a> {
        registry: &'a Registry,
        blocked: BTreeMap<String, String>,
        server: bool,
        include_disabled: bool,
        done: BTreeSet<String>,
        order: Vec<String>,
    }
    impl Visitor<'_> {
        fn visit(&mut self, id: &str, visiting: &mut BTreeSet<String>) -> Result<(), String> {
            if let Some(reason) = self.blocked.get(id) {
                return Err(format!("{id}: {reason}"));
            }
            if self.done.contains(id) {
                return Ok(());
            }
            if !visiting.insert(id.into()) {
                return Err(format!("dependency cycle: {id}"));
            }
            let m = &self
                .registry
                .get(id)
                .ok_or_else(|| format!("missing dependency: {id}"))?
                .parsed()
                .manifest;
            if !self.include_disabled && !m.enabled {
                return Err(format!("disabled dependency: {id}"));
            }
            if !m.target.supports_process(self.server) {
                return Err(format!("{id}: wrong process target"));
            }
            for dep in &m.dependencies {
                if dep.id == "shroudforge-api" {
                    if !dep.version.matches(&semver::Version::new(1, 0, 0)) {
                        return Err(format!("{id}: incompatible API dependency"));
                    }
                    continue;
                }
                let candidate = self.registry.get(&dep.id);
                if dep.optional.unwrap_or(false)
                    && (candidate.is_none()
                        || (!self.include_disabled
                            && candidate.is_some_and(|p| !p.parsed().manifest.enabled)))
                {
                    continue;
                }
                let candidate = candidate.ok_or_else(|| format!("{id}: missing {}", dep.id))?;
                if !dep.version.matches(&candidate.parsed().manifest.version) {
                    return Err(format!("{id}: incompatible {}", dep.id));
                }
                self.visit(&dep.id, visiting)?;
            }
            visiting.remove(id);
            self.done.insert(id.into());
            self.order.push(id.into());
            Ok(())
        }
    }
    let mut visitor = Visitor {
        registry,
        blocked,
        server,
        include_disabled,
        done: BTreeSet::new(),
        order: Vec::new(),
    };
    let mut issues = Vec::new();
    // Registry BTreeMap supplies the same lexical order as SF's sorted root IDs.
    for p in registry.values() {
        let m = &p.parsed().manifest;
        if (!include_disabled && !m.enabled) || !m.target.supports_process(server) {
            continue;
        }
        if let Err(error) = visitor.visit(&m.id, &mut BTreeSet::new()) {
            issues.push(PlanIssue::new(&m.id, error));
        }
    }
    Plan {
        ids: visitor.order,
        issues,
    }
}
