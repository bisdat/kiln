// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: the ShroudForge-dialect pass with its one retry, on the start's witness.
//!
//! Each attempt is a NEW `StartupHost` over the start's input witness (`open_matching`: an input that changed since the
//! witness refuses the attempt, and it is never recaptured), the install lock held across both (never released or
//! taken again), then the pass composition's `run_attempt` inside `catch_unwind` (a panic is a failure no package raised: never
//! retried). The retry driver decides the retry (`run_pass_with`): a package's own failure, not the system's, retried
//! once without it and what needs it, in the order ShroudForge's fork plans the retry (`StartupPlan::fork_order`). A
//! package's own missing entry module (`src/mod.lua`) is its own failure, retried, although the core reports it with
//! an OS error code, unlike any other failure that names one. DLLs are never
//! loaded again: a dropped package's DLL stays loaded and its Lua is withdrawn later (the runtime phase).
//!
//! Adapted from ShroudForge, Kiln's fork at 2df1de68 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_pass.rs):
//! retry_without_failed_mod.rs (the retry), plan_mod_load_order.rs (its order and closure).
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, BTreeSet, VecDeque},
    io,
    panic::{AssertUnwindSafe, catch_unwind},
};

use mod_loader::lua::startup_host::StartupHost;

use super::{
    plan::StartupPlan,
    runner::{self, PreparedStartup, SideEffects, run_attempt},
};
use crate::{
    sf_pass::{PackageFailure, PassError, PassPlan, Retry, StartupDialect, run_pass_with},
    startup::{Data, Point, Row, clamp},
};

/// What the pass did.
pub struct PassRun<'g> {
    /// The prepared generation of the attempt that succeeded, or the FIRST attempt's error (the fork's record).
    pub result: Result<PreparedStartup<'g>, PassError>,
    /// The retry's record (`skipped`, `retryError`).
    pub retry: Retry,
    /// How many attempts began (1, or 2 with the retry).
    pub attempts: u8,
    /// Each failed attempt's error, whole (`PassError`'s text: the fork's `LoaderError::Pregame`).
    pub errors: Vec<String>,
    /// The first attempt's plan (the retry's closure and chains are computed over it).
    pub first_plan: PassPlan,
    /// What the attempts entered and could export (kept even when they failed).
    pub effects: SideEffects,
}

/// Runs `plan`'s pass: `host` opens each attempt's new generation over the witness; `observe` gets every attempt and
/// step; `log` the fork's W lines.
pub fn run<'g>(
    plan: &StartupPlan,
    host: &dyn Fn() -> anyhow::Result<StartupHost<'g>>,
    vm: &runner::Ports,
    observe: &dyn Fn(Point<'_>),
    log: &dyn Fn(&str),
) -> PassRun<'g> {
    let first_plan = plan.pass_plan();
    let attempts = Cell::new(0u8);
    let errors = RefCell::new(Vec::new());
    let effects = RefCell::new(SideEffects::default());
    let attempt = |pass: &PassPlan| -> Result<PreparedStartup<'g>, PassError> {
        let n = attempts.get() + 1;
        attempts.set(n);
        observe(Point::Attempt(n));
        let result = catch_unwind(AssertUnwindSafe(|| {
            let mut host = host().map_err(PassError::Other)?;
            run_attempt(&mut host, plan, pass, vm, &mut effects.borrow_mut(), |step| {
                observe(Point::Step(step));
                Ok(())
            })
        }))
        .unwrap_or_else(|panic| {
            let text = panic
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| panic.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "a panic".into());
            Err(PassError::Other(anyhow::anyhow!("panic: {text}")))
        });
        if let Err(error) = &result {
            errors.borrow_mut().push(error.to_string());
        }
        result
    };
    let (result, retry) = run_pass_with(
        StartupDialect::Shroudforge,
        &first_plan,
        |failure| entry_missing(plan, failure),
        |kept| plan.fork_order(&kept),
        attempt,
        log,
    );
    PassRun {
        result,
        retry,
        attempts: attempts.get(),
        errors: errors.into_inner(),
        first_plan,
        effects: effects.into_inner(),
    }
}

/// The failing package's own entry module is missing: the core's text for it (`failed to load module: src/mod.lua
/// (...)`, with the OS error), and the package's own source has no `src/mod.lua` (not merely an unreadable one, which
/// stays the system's failure).
fn entry_missing(plan: &StartupPlan, failure: &PackageFailure) -> bool {
    !failure.io
        && failure.message.contains("failed to load module: src/mod.lua (")
        && plan.registry().get(&failure.mod_id).is_some_and(|record| {
            matches!(record.raw_source().metadata("src/mod.lua"), Err(e) if e.kind() == io::ErrorKind::NotFound)
        })
}

/// The rows a successful retry leaves for the packages it ran without: the failed package `failed`
/// (`startup-failed`, its one-line error), each package that needed it `excluded` (`dependency-skipped`, the fork's
/// text, its chain nearest first ending at the failed package). Empty without a retry.
pub fn skipped_rows(retry: &Retry, plan: &StartupPlan, first: &PassPlan) -> BTreeMap<String, Row> {
    let mut rows = BTreeMap::new();
    let Some(failed) = retry.skipped.first().and_then(|s| s["id"].as_str()) else {
        return rows;
    };
    let version = |id: &str| {
        plan.manifest(id)
            .map(|m| m.version.to_string())
            .filter(|v| !v.is_empty())
    };
    for (i, skipped) in retry.skipped.iter().enumerate() {
        let Some(id) = skipped["id"].as_str() else { continue };
        let text = clamp(skipped["error"].as_str().unwrap_or("failed in the pregame pass"), 4096);
        rows.insert(
            id.to_owned(),
            if i == 0 {
                Row {
                    id: id.to_owned(),
                    version: version(id),
                    data: Data::Failed,
                    archive_present: Some(false),
                    code: Some("startup-failed".into()),
                    text: Some(text),
                    chain: Vec::new(),
                }
            } else {
                Row {
                    id: id.to_owned(),
                    version: version(id),
                    data: Data::Excluded,
                    archive_present: Some(false),
                    code: Some("dependency-skipped".into()),
                    text: Some(text),
                    chain: chain(first, id, failed),
                }
            },
        );
    }
    rows
}

/// The dependency path from `id` to `failed` in `plan`, nearest first, without `id`: the edges the retry's closure
/// follows (required dependencies, and switched-on optional ones other than the failed package).
fn chain(plan: &PassPlan, id: &str, failed: &str) -> Vec<String> {
    let edges = |of: &str| -> Vec<String> {
        plan.packages()
            .iter()
            .find(|p| p.id == of)
            .map(|p| {
                p.requires
                    .iter()
                    .chain(p.optional.iter().filter(|o| *o != failed))
                    .cloned()
                    .collect()
            })
            .unwrap_or_default()
    };
    let mut previous: BTreeMap<String, String> = BTreeMap::new();
    let mut seen = BTreeSet::from([id.to_owned()]);
    let mut queue = VecDeque::from([id.to_owned()]);
    while let Some(at) = queue.pop_front() {
        if at == failed {
            let mut path = vec![at.clone()];
            let mut step = at;
            while let Some(back) = previous.get(&step) {
                if back == id {
                    break;
                }
                path.push(back.clone());
                step = back.clone();
            }
            path.reverse();
            path.truncate(64);
            return path;
        }
        for next in edges(&at) {
            if seen.insert(next.clone()) {
                previous.insert(next.clone(), at.clone());
                queue.push_back(next);
            }
        }
    }
    vec![failed.to_owned()]
}
