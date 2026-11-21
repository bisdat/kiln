// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: the SF-dialect startup pass driver: a pass that failed in one package's own entrypoint runs
//! once more, from the clean baseline, without that package and the packages that need it.
//!
//! It takes effect only when the frozen launch plan's top-level `startupDialect` is `shroudforge`: the EML dialect keeps
//! kiln-eml's own behaviour (runner.rs), untouched (RULES dialect rule 1). Each attempt is one whole pass the caller
//! supplies (`attempt`): a new `startup_host::StartupHost` generation over the same validated baseline, the packages,
//! and the sole publisher. A failed attempt published nothing (its private stage is dropped), so the retry starts from
//! the same clean baseline, as every SF pass does. Once only: no loop, no search.
//!
//! Adapted from ShroudForge, Kiln's fork at 2df1de68 (VERSION 1.7.9):
//! - modules/entrypoints/loader/asset_preparation/retry_without_failed_mod.rs: which failure is one package's, the retry,
//!   the record's `skipped`/`retryError` and the log texts;
//! - modules/entrypoints/loader/asset_preparation/apply_asset_mods.rs: one pass per environment, the retry's place;
//! - modules/mod-runtime/startup/name_failing_mod.rs: the failing package and the I/O error in its chain;
//! - modules/modloader/plan_mod_load_order.rs: `ModEnvironment::without` (the package and what needs it drop out).
//!
//! Those parts are:
//!   MIT License
//!
//!   Copyright (c) 2026 bonsaibauer
//!
//!   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//!   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//!   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
//!   permit persons to whom the Software is furnished to do so, subject to the following conditions:
//!
//!   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
//!   the Software.
//!
//!   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//!   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//!   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//!   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//!   SOFTWARE.
use std::{collections::BTreeSet, error::Error, fmt};

/// The frozen launch plan's top-level `startupDialect` (launch-plan v1), as handed to this component.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StartupDialect {
    Shroudforge,
    Eml,
    None,
}
impl StartupDialect {
    /// The plan's spelling; anything else is not a dialect.
    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "shroudforge" => Some(Self::Shroudforge),
            "eml" => Some(Self::Eml),
            "none" => Some(Self::None),
            _ => None,
        }
    }
}

/// The error of a package or the record is cut to this many characters (fork `MAX_ERROR`).
const MAX_ERROR: usize = 400;

/// One package of the frozen pass plan, in load order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlannedPackage {
    pub id: String,
    pub name: String,
    pub version: String,
    /// The ids of the packages it needs (required dependencies; optional ones are not listed).
    pub requires: Vec<String>,
    /// The ids of its optional dependencies that are installed and switched on (the launch plan's included optional
    /// edges). The fork plans such a dependency before the package, so the package drops out when that dependency drops
    /// out because it needs the failed package; an optional dependency on the failed package itself is skipped, as on
    /// any package that is switched off (plan_mod_load_order.rs `visit`).
    pub optional: Vec<String>,
    /// It has a part the pregame pass runs (SF `requires_pregame`: patch, or export without a runtime part).
    pub pregame: bool,
}

/// The packages of one pass, in load order. A retry only ever removes packages: no other package, version or dialect.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PassPlan {
    packages: Vec<PlannedPackage>,
}
impl PassPlan {
    pub fn new(packages: Vec<PlannedPackage>) -> Self {
        Self { packages }
    }
    pub fn packages(&self) -> &[PlannedPackage] {
        &self.packages
    }
    /// This plan with `id` treated as switched off: it and every package that needs it, directly or through another,
    /// drop out (SF's `dependency-disabled`). As in the fork's planner (plan_mod_load_order.rs `visit`), a package
    /// drops out when a required dependency drops out, or a switched-on optional one other than `id` (planned before
    /// it, that one fails); an optional dependency on `id` itself is skipped, as on any switched-off package.
    ///
    /// The packages kept stay in this plan's order. The fork plans the retry anew instead (switched-on packages as
    /// roots in id order, each after its dependencies), which can order them differently when the failed package
    /// pulled a dependency ahead of its own place; the caller that runs the real pass owns that order.
    pub fn without(&self, id: &str) -> Self {
        let mut off = BTreeSet::from([id.to_owned()]);
        loop {
            let before = off.len();
            for package in &self.packages {
                if package.requires.iter().any(|r| off.contains(r))
                    || package.optional.iter().any(|o| o != id && off.contains(o))
                {
                    off.insert(package.id.clone());
                }
            }
            if off.len() == before {
                break;
            }
        }
        Self {
            packages: self
                .packages
                .iter()
                .filter(|p| !off.contains(&p.id))
                .cloned()
                .collect(),
        }
    }
    fn pregame(&self) -> impl Iterator<Item = &PlannedPackage> {
        self.packages.iter().filter(|p| p.pregame)
    }
}

/// The error one package's own entrypoint raised while the pass ran it (fork `PregameModError`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackageFailure {
    /// The manifest id of the package whose entrypoint failed.
    pub mod_id: String,
    /// The error as the Lua runtime gave it, whole (with its stack traceback).
    pub message: String,
    /// An I/O error is in the error's chain: the system's or the files' failure, not the package's.
    pub io: bool,
}
impl PackageFailure {
    /// `links`: the error's chain as its Lua runtime walks it (mlua's `Error::chain()`, whose `source()` stops at a
    /// callback error), each link's `source()` chain followed too, so an I/O error behind a context counts.
    pub fn new<'a>(
        mod_id: &str,
        message: impl Into<String>,
        links: impl IntoIterator<Item = &'a (dyn Error + 'static)>,
    ) -> Self {
        let io = links.into_iter().any(|link| {
            let mut current = Some(link);
            while let Some(error) = current {
                if error.is::<std::io::Error>() {
                    return true;
                }
                current = error.source();
            }
            false
        });
        Self {
            mod_id: mod_id.to_owned(),
            message: message.into(),
            io,
        }
    }
}

/// Why one attempt of the pass failed.
#[derive(Debug)]
pub enum PassError {
    /// Raised by one package's entrypoint.
    Package(PackageFailure),
    /// Anything no package raised: the working baseline, staging, the type model, the commit and publication, a
    /// replaced `.kfc`. Never retried.
    Other(anyhow::Error),
}
impl fmt::Display for PassError {
    /// As the fork's loader records it (`LoaderError::Pregame`).
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PassError::Package(failure) => write!(f, "pregame phase failed: {}", failure.message),
            PassError::Other(error) => write!(f, "pregame phase failed: {error:#}"),
        }
    }
}
impl Error for PassError {}

/// What the retry adds to the record of the pass (state "assets"; the interpreter's own state file later carries the
/// same shape).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Retry {
    /// The failed package first, then the packages left out because they need it.
    pub skipped: Vec<serde_json::Value>,
    /// The second pass's error, when it failed too.
    pub error: Option<String>,
}
impl Retry {
    /// Adds the retry's fields to a record of the pass (nothing when there was no retry).
    pub fn record_into(&self, record: &mut serde_json::Value) {
        if !self.skipped.is_empty() {
            record["skipped"] = serde_json::Value::Array(self.skipped.clone());
        }
        if let Some(error) = &self.error {
            record["retryError"] = error.as_str().into();
        }
    }
}

/// The package a failed pass is pinned on.
struct FailedMod {
    id: String,
    name: String,
    version: String,
    /// Its error on one line, without the Lua stack traceback.
    error: String,
}
impl FailedMod {
    /// "Wisp Light Timer (wisp-light-timer 1.0.3)", or "wisp-light-timer 1.0.3" when the name is the id.
    fn label(&self) -> String {
        if self.name == self.id || self.name.is_empty() {
            format!("{} {}", self.id, self.version)
        } else {
            format!("{} ({} {})", self.name, self.id, self.version)
        }
    }
}

/// The error on one line: the Lua stack traceback cut off, white space collapsed, at most [`MAX_ERROR`] characters.
pub(crate) fn one_line(text: &str) -> String {
    let text = text.split("stack traceback:").next().unwrap_or(text);
    let line = text.split_whitespace().collect::<Vec<_>>().join(" ");
    if line.chars().count() > MAX_ERROR {
        return line.chars().take(MAX_ERROR).collect::<String>() + "...";
    }
    line
}

/// A failure of the system or of the game's files, not of one package: an I/O error in the chain, or a text with
/// Rust's "(os error N)" (in every language) or an access-denied text of Windows, Wine or Unix.
fn system_failure(error: &PackageFailure) -> bool {
    let lower = error.message.to_lowercase();
    error.io
        || [
            "(os error",
            "access denied",
            "access is denied",
            "permission denied",
        ]
        .iter()
        .any(|t| lower.contains(t))
}

/// The package the failed pass is pinned on: an error its own entrypoint raised, no failure of the system or the game's
/// files (unless `own` says the failure is the package's own whatever its text: the startup controller), and the package is in this
/// pass's plan.
fn failed_mod(error: &PassError, plan: &PassPlan, own: &dyn Fn(&PackageFailure) -> bool) -> Option<FailedMod> {
    let PassError::Package(failure) = error else {
        return None;
    };
    if !own(failure) && system_failure(failure) {
        return None;
    }
    let package = plan.packages.iter().find(|p| p.id == failure.mod_id)?;
    Some(FailedMod {
        id: failure.mod_id.clone(),
        name: package.name.clone(),
        version: package.version.clone(),
        error: one_line(&failure.message),
    })
}

/// "the other 3 mods loaded", "the other mod loaded", "no other mod needs the pregame pass".
fn others(count: usize) -> String {
    match count {
        0 => "no other mod needs the pregame pass".into(),
        1 => "the other mod loaded".into(),
        n => format!("the other {n} mods loaded"),
    }
}

/// Runs the startup pass for `plan`. `attempt` is one whole pass from the clean baseline (a new StartupHost
/// generation, the packages, publication). In the SF dialect a failure one package's entrypoint raised runs `attempt`
/// once more without that package and the packages that need it; `log` gets the fork's W lines (category
/// `startup-assets`). Returns the outcome the record keeps (on a second failure: the FIRST error, as the fork records
/// it) and the retry's record fields. Any other dialect runs `attempt` once and adds nothing.
///
/// The caller holds the install lock across both attempts (as `lock_game` holds it for the process's life) and never
/// drops and takes it again for the retry: on Unix `flock` belongs to the open file description, so a process spawned
/// meanwhile by any thread can still hold the lock through its inherited descriptor until its exec.
pub fn run_pass<T>(
    dialect: StartupDialect,
    plan: &PassPlan,
    attempt: impl FnMut(&PassPlan) -> Result<T, PassError>,
    log: impl FnMut(&str),
) -> (Result<T, PassError>, Retry) {
    run_pass_with(dialect, plan, |_| false, |kept| kept, attempt, log)
}

/// `run_pass` with the startup controller's two refinements:
/// - `own`: a failure that is the failing package's own whatever its text says, classified BEFORE the system-failure
///   rule (the package's own entry module is missing, which the core reports with an OS error code);
/// - `replan`: the order the retry runs the packages it keeps in (the fork plans its retry anew, see
///   `PassPlan::without`); it may only reorder them. The record and the log lines stay the plan's.
pub fn run_pass_with<T>(
    dialect: StartupDialect,
    plan: &PassPlan,
    own: impl Fn(&PackageFailure) -> bool,
    replan: impl FnOnce(PassPlan) -> PassPlan,
    mut attempt: impl FnMut(&PassPlan) -> Result<T, PassError>,
    mut log: impl FnMut(&str),
) -> (Result<T, PassError>, Retry) {
    let first = attempt(plan);
    if dialect != StartupDialect::Shroudforge {
        return (first, Retry::default());
    }
    let first = match first {
        Ok(value) => return (Ok(value), Retry::default()),
        Err(error) => error,
    };
    let Some(failed) = failed_mod(&first, plan, &own) else {
        return (Err(first), Retry::default());
    };
    log(&format!(
        "ShroudForge: {} failed in the pregame pass ({}); running the pass once more without it",
        failed.label(),
        failed.error
    ));
    let without = plan.without(&failed.id);
    // the pregame packages that need the failed one drop out of the plan with it
    let dependents: Vec<&PlannedPackage> = plan
        .pregame()
        .filter(|p| p.id != failed.id && !without.packages.iter().any(|kept| kept.id == p.id))
        .collect();
    let mut retry = Retry::default();
    retry.skipped.push(serde_json::json!({
        "id": failed.id, "name": failed.name, "version": failed.version, "error": failed.error
    }));
    for package in &dependents {
        retry.skipped.push(serde_json::json!({
            "id": package.id, "name": package.name, "version": package.version,
            "error": format!("needs {}, which failed in the pregame pass", failed.id)
        }));
    }
    let with_it = if dependents.is_empty() {
        String::new()
    } else {
        format!(
            " and {} ({})",
            if dependents.len() == 1 {
                "the mod that needs it".to_string()
            } else {
                format!("the {} mods that need it", dependents.len())
            },
            dependents
                .iter()
                .map(|p| p.id.as_str())
                .collect::<Vec<_>>()
                .join(", ")
        )
    };
    // a planner that would add, drop or repeat a package is not used: the kept packages in the plan's order instead
    let ordered = Some(replan(without.clone())).filter(|o| {
        o.packages.len() == without.packages.len() && without.packages.iter().all(|p| o.packages.contains(p))
    });
    match attempt(ordered.as_ref().unwrap_or(&without)) {
        Ok(value) => {
            log(&format!(
                "ShroudForge: {} failed in the pregame pass ({}); started the game without it{with_it}, {}",
                failed.label(),
                failed.error,
                others(without.pregame().count())
            ));
            (Ok(value), retry)
        }
        Err(second) => {
            log(&format!(
                "ShroudForge: the pregame pass failed again without {} ({}); the game starts without the mods of the pregame pass",
                failed.label(),
                one_line(&second.to_string())
            ));
            retry.error = Some(second.to_string());
            (Err(first), retry)
        }
    }
}

/// `run_pass`'s log lines in kiln.log: warnings with the fork's category as their target.
pub fn log_warning(line: &str) {
    tracing::warn!(target: "startup-assets", "{line}");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_error_is_one_line_without_the_traceback() {
        assert_eq!(
            one_line(
                "runtime error: [string \"wisp\"]:744: Missing base localization: keenglish\nstack traceback:\n\t[C]: in ?"
            ),
            "runtime error: [string \"wisp\"]:744: Missing base localization: keenglish"
        );
        assert_eq!(one_line("a\r\n  b\tc"), "a b c");
        assert_eq!(
            one_line(&"x".repeat(MAX_ERROR + 9)).chars().count(),
            MAX_ERROR + 3
        );
    }

    fn raised(message: &str, io: bool) -> PackageFailure {
        PackageFailure {
            mod_id: "wisp".into(),
            message: message.into(),
            io,
        }
    }

    #[test]
    fn failures_of_the_system_or_the_game_files_are_not_one_packages() {
        for message in [
            "runtime error: could not open enshrouded.kfc_resources: Access is denied. (os error 5)",
            "runtime error: Accès refusé. (os error 5)",
            "runtime error: The process cannot access the file because it is being used by another process. (os error 32)",
            "runtime error: Access denied",
            "runtime error: Permission denied",
        ] {
            assert!(system_failure(&raised(message, false)), "{message}");
        }
        assert!(system_failure(&raised("failed to fill whole buffer", true)));
        assert!(!system_failure(&raised(
            "runtime error: [string \"wisp\"]:719: Missing base localization: keenglish",
            false
        )));
    }

    fn package(id: &str, requires: &[&str], optional: &[&str]) -> PlannedPackage {
        PlannedPackage {
            id: id.into(),
            name: id.into(),
            version: "1.0.0".into(),
            requires: requires.iter().map(|r| r.to_string()).collect(),
            optional: optional.iter().map(|r| r.to_string()).collect(),
            pregame: true,
        }
    }
    fn ids(plan: &PassPlan) -> Vec<String> {
        plan.packages().iter().map(|p| p.id.clone()).collect()
    }

    #[test]
    fn a_plan_without_a_package_drops_what_needs_it_directly_or_through_another() {
        let plan = PassPlan::new(vec![
            package("base", &[], &[]),
            package("top", &["middle"], &[]),
            package("middle", &["base"], &[]),
            package("other", &[], &[]),
        ]);
        assert_eq!(ids(&plan.without("base")), ["other"]);
        assert_eq!(ids(&plan.without("middle")), ["base", "other"]);
        assert_eq!(ids(&plan.without("other")), ["base", "top", "middle"]);
    }

    #[test]
    fn an_optional_dependency_drops_a_package_only_when_it_drops_out_with_the_failed_one() {
        // fork plan_mod_load_order.rs `visit`: an optional dependency that is switched off (the failed package, in the
        // retry) is skipped; one that is switched on is planned first, and when it cannot be, neither can the package
        let plan = PassPlan::new(vec![
            package("base", &[], &[]),
            package("middle", &["base"], &[]),
            package("uses-base", &[], &["base"]),
            package("uses-middle", &[], &["middle"]),
            package("through-optional", &["uses-middle"], &[]),
            package("other", &[], &["not-installed"]),
        ]);
        assert_eq!(ids(&plan.without("base")), ["uses-base", "other"]);
        assert_eq!(
            ids(&plan.without("middle")),
            [
                "base",
                "uses-base",
                "uses-middle",
                "through-optional",
                "other"
            ]
        );
    }
}
