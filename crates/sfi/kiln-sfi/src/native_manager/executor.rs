// SPDX-License-Identifier: MIT
//! Guarded native preparation and terminal admission.
//!
//! ShroudForge loads its applied DLL plan on one ordered process worker and waits up to 15 s
//! before releasing the game (1.7.9 `modules/mod-runtime/native_dlls.rs:12,212-299`; 1.8.0 the
//! same). Kiln keeps that shape and adds the rules ShroudForge does not have:
//! - nothing loads before the startup witness (the guarded pre-native baseline) is ready
//!   for this plan; without it every DLL is refused and the backend is never called;
//! - each DLL is re-established (same file, same SHA-256 as planned), gated and bound by the native-load enforcement's
//!   [`LoadPermit`] and loaded through its [`Enforcer`]: first loader wins, faults pin and
//!   quarantine, nothing is ever retried or unloaded;
//! - a package runs whole: its first failure withdraws its remaining DLLs and every planned
//!   dependent, while independent packages go on;
//! - Kiln's native.policy decides at boot: when it refuses (`never`, or
//!   `verified-only` on a build that is not verified), every package with a planned DLL is
//!   withdrawn in Kiln's words, with its dependents, and the loader is never asked;
//! - every planned DLL gets exactly one terminal record. When the deadline passes, unfinished
//!   ones are `TimedOut`, no new load starts (the commit point is the loader call itself, after
//!   every check), and a load that finishes later is logged (the enforcement's record, kiln.log),
//!   never admitted. A load committed just before the run closed can enter the loader just after
//!   it: its record is `TimedOut` all the same. The loader thread is left to finish its call, as
//!   ShroudForge leaves its worker: never stopped inside a DLL's code;
//! - the run has exactly one terminal: whoever concludes it first (the loader's side once every
//!   DLL is terminal, the deadline waiter at ShroudForge's deadline, any later `wait`) builds it
//!   and rechecks the witness once, and every other caller gets that same terminal. No caller
//!   waits for another's recheck longer than [`RECHECK_ALLOWANCE`]: then the terminal is
//!   published not coherent and stays the run's, also for the caller still inside the recheck;
//! - after the terminals the same witness is rechecked: a DLL that changed the witnessed input
//!   makes the run incoherent (the startup pass must refuse to publish, never bless it);
//! - a run that is never armed (the policy refuses, the plan has no DLL, no witness is wired,
//!   or the run was made after the early phase) is concluded by
//!   [`NativeRun::conclude_without_loader`]: every planned DLL is refused with that reason and
//!   nothing reaches the loader, the notifier or a thread.
//!
//! The caller picks the thread for [`NativeRun::execute`] (the loader worker, see
//! [`super::arming`]) and calls [`NativeRun::wait`] from the startup side, or
//! [`NativeRun::expire`] from the deadline waiter; this file starts no thread.
use super::{
    plan::{NativePlan, PlannedDll},
    selection::{Mode, NativePermission, Withdrawn},
};
use crate::{
    native_contracts::NativeContracts,
    native_enforcement::{Backend, Enforcer, LoadOutcome, Sink, Snapshot},
    pe_gate,
};
use std::{
    collections::{BTreeMap, BTreeSet},
    panic::{AssertUnwindSafe, catch_unwind},
    path::Path,
    sync::{Arc, Condvar, Mutex},
    time::{Duration, Instant},
};

/// ShroudForge's startup wait for the applied DLL plan.
pub const STARTUP_NATIVE_DEADLINE: Duration = Duration::from_secs(15);

/// The launch-scoped token of the guarded startup witness (the startup pass's opaque lease, reduced to what
/// the native side checks).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WitnessToken {
    pub plan_id: [u8; 16],
    pub revision: u64,
}
/// The startup pass's guard producer: kiln-eml's startup seam in production, a fake in tests.
pub trait Witness: Send + Sync {
    /// The witness for this launch, issued under the install guard before any package DLL
    /// initializes. An error means it is not ready: no DLL may load.
    fn ready(&self) -> Result<WitnessToken, String>;
    /// After the native terminals: the witnessed input is unchanged.
    fn recheck(&self, token: &WitnessToken) -> Result<(), String>;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// This run loaded it (its DllMain ran without a fault).
    Loaded { module: usize },
    /// Another loader had already loaded the same file: never initialized again.
    ForeignOwned { module: usize },
    /// Refused before it could run: witness, identity, mode, gate or an ordinary loader refusal.
    Refused(String),
    /// The load faulted or its initializer failed: pinned and quarantined, never retried.
    Faulted(String),
    /// Not finished when the startup deadline passed.
    TimedOut(String),
    /// Not attempted: an earlier DLL of its package or a dependency failed.
    Withdrawn(String),
}
impl Outcome {
    pub fn admitted(&self) -> bool {
        matches!(self, Self::Loaded { .. } | Self::ForeignOwned { .. })
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TerminalRecord {
    pub status_id: String,
    pub mod_id: String,
    pub package_directory: String,
    pub path: String,
    pub planned_sha256: String,
    /// The digest of the bytes actually captured for the load, when it got that far.
    pub actual_sha256: Option<String>,
    pub mode: Mode,
    pub source: String,
    pub outcome: Outcome,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NativeTerminal {
    /// Exactly one per planned DLL, in plan order.
    pub records: Vec<TerminalRecord>,
    /// Packages that will not run: a failed DLL, or a withdrawn dependency.
    pub withdrawn: Vec<Withdrawn>,
    /// The witness recheck after the terminals. A run concluded without the loader loaded nothing,
    /// so the native side changed nothing a witness covers: `Ok`.
    pub coherent: Result<(), String>,
    pub deadline_passed: bool,
    /// The loader was never armed this start, and why (`NativeRun::conclude_without_loader`): no
    /// notifier, no thread, no capture and no load; every record is `Refused` with this reason.
    pub not_armed: Option<String>,
}
impl NativeTerminal {
    pub fn runs_whole(&self, mod_id: &str) -> bool {
        !self.withdrawn.iter().any(|w| w.mod_id == mod_id)
    }
    /// The record behind a process key (`<folder>/<path>`, case-insensitive), for register_dll.
    pub fn record_for_key(&self, key: &str) -> Option<&TerminalRecord> {
        self.records
            .iter()
            .find(|r| process_key(&r.package_directory, &r.path) == key.to_ascii_lowercase())
    }
}
fn process_key(directory: &str, path: &str) -> String {
    let path = path
        .split('/')
        .filter(|c| !c.is_empty())
        .collect::<Vec<_>>()
        .join("/");
    format!("{directory}/{path}").to_ascii_lowercase()
}

#[derive(Default)]
struct Progress {
    outcomes: Vec<Option<Outcome>>,
    actual: Vec<Option<String>>,
    started: bool,
    finished: bool,
    closed: bool,
    /// The DLL the loader was told to start (committed under this lock before the deadline).
    loading: Option<usize>,
    /// Concluded without the loader: why (`NativeRun::conclude_without_loader`).
    not_armed: Option<String>,
    token: Option<WitnessToken>,
    late: Vec<(String, Outcome)>,
    /// One caller is building the terminal (its witness recheck runs without the lock); every
    /// other caller waits for that terminal instead of building its own, at most until this
    /// conclusion's `until` ([`RECHECK_ALLOWANCE`] after it began).
    concluding: Option<Concluding>,
    /// The terminal was published by a caller that stopped waiting for a recheck that did not
    /// answer ([`RECHECK_UNANSWERED`]): the deadline waiter pushes it too, whenever it was concluded.
    unanswered: bool,
    terminal: Option<NativeTerminal>,
}
/// The terminal one caller is concluding, built under the lock but for its witness recheck,
/// and when the other callers stop waiting for that recheck.
struct Concluding {
    terminal: NativeTerminal,
    until: Instant,
}
/// Who asks [`NativeRun::settle`] for the terminal.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Asker {
    /// [`NativeRun::wait`]: the terminal once every DLL is terminal or the deadline passed.
    Any,
    /// [`NativeRun::expire`]: a terminal only when the deadline passed before every DLL was terminal.
    Late,
}
/// The coherence of a run whose witness recheck panicked (fail closed).
const RECHECK_FAILED: &str = "the startup witness recheck failed with an internal error";
/// How long a caller waits for the terminal another caller is concluding (that caller's witness recheck runs
/// without the lock) before it publishes that terminal without the recheck's answer, not coherent
/// ([`RECHECK_UNANSWERED`]). kiln-eml waits for the native terminal at most attach + 20 s, and ShroudForge's 15 s
/// deadline counts from the arming, after the attach: a recheck still unanswered 5 s after the run's conclusion began
/// cannot reach kiln-eml inside its wait. The recheck is kiln-eml's call, which nothing here can stop.
pub const RECHECK_ALLOWANCE: Duration = Duration::from_secs(5);
/// The coherence of a run whose witness recheck did not answer within [`RECHECK_ALLOWANCE`] (fail closed).
pub const RECHECK_UNANSWERED: &str = "the startup witness recheck did not answer within 5 s of the native run's conclusion, so the input it witnessed cannot be confirmed unchanged after the DLLs";
/// One launch's native run. Shared between the loader worker and the startup waiter.
pub struct NativeRun {
    plan: NativePlan,
    /// Kiln's native.policy at boot (the record's frozen policy and `build_verified`).
    permission: NativePermission,
    index: Vec<(usize, usize)>,
    witness: Arc<dyn Witness>,
    progress: Mutex<Progress>,
    changed: Condvar,
}
impl NativeRun {
    /// `permission`: Kiln's native.policy for this start, as kiln-sfi read it at boot (kiln.launch's frozen policy and
    /// `build_verified`). Boot wins over the plan: when it refuses, nothing in the plan loads.
    pub fn new(
        plan: NativePlan,
        witness: Arc<dyn Witness>,
        permission: NativePermission,
    ) -> Arc<Self> {
        let index: Vec<_> = plan
            .packages
            .iter()
            .enumerate()
            .flat_map(|(p, package)| (0..package.dlls.len()).map(move |d| (p, d)))
            .collect();
        Arc::new(Self {
            progress: Mutex::new(Progress {
                outcomes: vec![None; index.len()],
                actual: vec![None; index.len()],
                ..Progress::default()
            }),
            plan,
            permission,
            index,
            witness,
            changed: Condvar::new(),
        })
    }
    fn lock(&self) -> std::sync::MutexGuard<'_, Progress> {
        self.progress.lock().unwrap_or_else(|e| e.into_inner())
    }
    /// Kiln's native.policy this run was made with.
    pub fn permission(&self) -> NativePermission {
        self.permission
    }
    /// How many DLLs the plan lists (0: a plan with no native part).
    pub fn dll_count(&self) -> usize {
        self.index.len()
    }
    /// The launch plan's id (the witness must name it).
    pub fn plan_id(&self) -> [u8; 16] {
        self.plan.plan_id
    }
    /// The run is concluded without ever arming the loader (the policy refuses, the plan has
    /// no DLL, or no startup witness is wired): every planned DLL is `Refused` with `reason`, every
    /// package with a planned DLL is withdrawn with `reason` and its dependents (Lua-only ones
    /// included) with theirs. Nothing is captured, hashed or loaded, and the terminal is there at
    /// once, so [`Self::wait`] never waits. False, and nothing changes, when the run was started or
    /// concluded already; a later [`Self::execute`] does nothing.
    pub fn conclude_without_loader(&self, reason: &str) -> bool {
        let mut progress = self.lock();
        if progress.started
            || progress.finished
            || progress.concluding.is_some()
            || progress.terminal.is_some()
        {
            return false;
        }
        progress.started = true;
        progress.finished = true;
        progress.closed = true;
        progress.not_armed = Some(reason.to_owned());
        for outcome in &mut progress.outcomes {
            *outcome = Some(Outcome::Refused(reason.to_owned()));
        }
        let records = self.records(&progress);
        let withdrawn = self.withdrawn(&records, Some(reason));
        progress.terminal = Some(NativeTerminal {
            records,
            withdrawn,
            coherent: Ok(()),
            deadline_passed: false,
            not_armed: Some(reason.to_owned()),
        });
        self.changed.notify_all();
        true
    }
    /// The loader thread could not run the plan after all (its backend or the early native-load enforcement
    /// refused): nothing loads, every DLL not yet terminal is `Refused` with `reason`. A later
    /// [`Self::execute`] does nothing. Idempotent.
    pub fn abandon(&self, reason: &str) {
        self.lock().started = true;
        self.refuse_all(reason);
    }
    /// Run the plan once on the calling thread (the loader worker). A second call does nothing:
    /// a native run is never retried or re-initialized.
    pub fn execute<B: Backend, S: Sink + Send + 'static>(
        &self,
        enforcer: &mut Enforcer<B, S>,
        registry: &NativeContracts,
    ) {
        {
            let mut progress = self.lock();
            if progress.started {
                return;
            }
            progress.started = true;
            if progress.closed {
                // The startup side already gave up: start nothing.
                progress.finished = true;
                return;
            }
        }
        let token = match self.witness.ready() {
            Ok(token) if token.plan_id == self.plan.plan_id => token,
            Ok(_) => return self.refuse_all("the startup witness belongs to another launch plan"),
            Err(error) => {
                return self.refuse_all(&format!(
                    "the startup witness is not ready ({error}); no package DLL runs before the guarded baseline"
                ));
            }
        };
        self.lock().token = Some(token);
        let mut failed: BTreeMap<String, String> = BTreeMap::new();
        // Boot wins over the plan: a plan written while the policy allowed DLLs loads none of them now.
        if let Some(text) = self.permission.refusal() {
            for package in self.plan.packages.iter().filter(|p| !p.dlls.is_empty()) {
                failed.insert(package.mod_id.clone(), text.to_owned());
            }
        }
        // Packages before this index have had their dependencies' withdrawal applied.
        let mut propagated = 0;
        for (position, &(p, d)) in self.index.iter().enumerate() {
            // A withdrawal travels through every planned package in plan order, Lua-only ones
            // included, so a DLL whose package needs a failed one only transitively never runs.
            // The plan orders every included dependency first.
            while propagated <= p {
                let package = &self.plan.packages[propagated];
                if !failed.contains_key(&package.mod_id)
                    && let Some(dep) = package
                        .dependencies
                        .iter()
                        .find(|dep| failed.contains_key(*dep))
                {
                    failed.insert(
                        package.mod_id.clone(),
                        format!("depends on withdrawn package {dep}"),
                    );
                }
                propagated += 1;
            }
            let package = &self.plan.packages[p];
            let dll = &package.dlls[d];
            {
                let mut progress = self.lock();
                if progress.closed {
                    break;
                }
                if let Some(cause) = failed.get(&package.mod_id) {
                    progress.outcomes[position] = Some(Outcome::Withdrawn(cause.clone()));
                    self.changed.notify_all();
                    continue;
                }
            }
            let owner = p as u64 + 1;
            // The commit point: asked after every check, right before the loader is called. Once
            // the startup side has closed the run, no new load starts.
            let start = || {
                let mut progress = self.lock();
                if progress.closed {
                    return false;
                }
                progress.loading = Some(position);
                true
            };
            let (outcome, actual) = load_one(enforcer, registry, &package.root, dll, owner, start);
            let mut progress = self.lock();
            // the loader was called for this DLL (the commit point said yes)
            let called = progress.loading.take() == Some(position);
            progress.actual[position] = actual;
            if !outcome.admitted() {
                failed
                    .entry(package.mod_id.clone())
                    .or_insert_with(|| format!("{} did not load", dll.path));
            }
            let late = if progress.outcomes[position].is_some() {
                // The deadline already made this DLL terminal: keep the record, log the result (a DLL
                // the loader was never called for ran nothing, and its record says so already).
                let line = called.then(|| late_line(dll, &outcome));
                progress.late.push((dll.status_id.clone(), outcome));
                line
            } else {
                progress.outcomes[position] = Some(outcome);
                None
            };
            self.changed.notify_all();
            drop(progress);
            if let Some(line) = late {
                enforcer.record(&line);
            }
        }
        let mut progress = self.lock();
        progress.finished = true;
        self.changed.notify_all();
    }
    fn refuse_all(&self, reason: &str) {
        let mut progress = self.lock();
        for outcome in &mut progress.outcomes {
            outcome.get_or_insert_with(|| Outcome::Refused(reason.into()));
        }
        progress.finished = true;
        self.changed.notify_all();
    }
    /// Wait until every DLL is terminal or `deadline` passes, then recheck the witness. Called
    /// from the startup side; idempotent (later calls return the same terminal).
    pub fn wait(&self, deadline: Duration) -> NativeTerminal {
        self.settle(Instant::now() + deadline, Asker::Any)
            .expect("a wait always ends with the run's terminal")
    }
    /// The deadline waiter of an armed run: blocks until every planned DLL is terminal or
    /// `until` (ShroudForge's deadline, counted from the arming) passes, whichever comes first.
    /// `None` when every DLL was terminal in time: the loader thread concludes the run and pushes
    /// its terminal. Otherwise the run is concluded here exactly as [`Self::wait`] does at its
    /// deadline (unfinished DLLs `TimedOut`, the run closed: no load starts afterwards; a load
    /// still running is left to finish and its result only logged) and that terminal is returned.
    /// A terminal another caller concluded past its own deadline is returned too, never one every
    /// DLL reached in time, unless that terminal was published because the recheck of the caller
    /// concluding it did not answer within [`RECHECK_ALLOWANCE`] (that caller may never push it).
    pub fn expire(&self, until: Instant) -> Option<NativeTerminal> {
        self.settle(until, Asker::Late)
    }
    /// The one way to the run's terminal. The first caller that finds every DLL terminal, or its
    /// `until` passed, claims the conclusion (`concluding`) under the lock and builds the terminal;
    /// every caller that comes while it is being built waits for it, so the run has exactly one
    /// terminal, rechecked once. That wait is bounded: when the concluding caller's recheck has not
    /// answered [`RECHECK_ALLOWANCE`] after the conclusion began, the waiting caller publishes the
    /// terminal not coherent ([`RECHECK_UNANSWERED`]) and the concluding caller, when its recheck
    /// answers at last, gets that same terminal.
    fn settle(&self, until: Instant, asker: Asker) -> Option<NativeTerminal> {
        let mut progress = self.lock();
        loop {
            if let Some(terminal) = &progress.terminal {
                let theirs = asker == Asker::Any || terminal.deadline_passed || progress.unanswered;
                return theirs.then(|| terminal.clone());
            }
            if let Some(concluding) = &progress.concluding {
                let left = concluding.until.saturating_duration_since(Instant::now());
                if left.is_zero() {
                    // The concluding caller is stuck in its recheck (a foreign call nothing here can
                    // stop): its terminal is the run's, not coherent.
                    let terminal = NativeTerminal {
                        coherent: Err(RECHECK_UNANSWERED.into()),
                        ..concluding.terminal.clone()
                    };
                    progress.terminal = Some(terminal.clone());
                    progress.unanswered = true;
                    self.changed.notify_all();
                    return Some(terminal);
                }
                progress = self
                    .changed
                    .wait_timeout(progress, left)
                    .unwrap_or_else(|e| e.into_inner())
                    .0;
                continue;
            }
            if progress.finished || progress.outcomes.iter().all(Option::is_some) {
                if asker == Asker::Late {
                    return None;
                }
                break;
            }
            let left = until.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            progress = self
                .changed
                .wait_timeout(progress, left)
                .unwrap_or_else(|e| e.into_inner())
                .0;
        }
        let deadline_passed = progress.outcomes.iter().any(Option::is_none);
        progress.closed = true;
        let loading = progress.loading;
        for (position, outcome) in progress.outcomes.iter_mut().enumerate() {
            if outcome.is_none() {
                let (p, d) = self.index[position];
                let dll = &self.plan.packages[p].dlls[d];
                *outcome = Some(Outcome::TimedOut(if loading == Some(position) {
                    format!(
                        "Native DLL '{}' did not finish initialization within 15 seconds. Disable it in DLL Manager and restart the game",
                        dll.path
                    )
                } else {
                    "not started: the native startup deadline passed".into()
                }));
            }
        }
        let records = self.records(&progress);
        let withdrawn = self.withdrawn(&records, self.permission.refusal());
        let token = progress.token.clone();
        let mut terminal = NativeTerminal {
            records,
            withdrawn,
            coherent: Ok(()),
            deadline_passed,
            not_armed: None,
        };
        progress.concluding = Some(Concluding {
            terminal: terminal.clone(),
            until: Instant::now() + RECHECK_ALLOWANCE,
        });
        // The recheck runs without the progress lock: it reads files, never this state. A recheck
        // that panics confirms nothing (and must not leave the other callers waiting).
        drop(progress);
        terminal.coherent = match &token {
            Some(token) => catch_unwind(AssertUnwindSafe(|| self.witness.recheck(token)))
                .unwrap_or_else(|_| Err(RECHECK_FAILED.into())),
            None => Err("no startup witness was issued".into()),
        };
        let mut progress = self.lock();
        // a caller that stopped waiting for this recheck published the run's terminal: it stays
        let terminal = progress.terminal.get_or_insert(terminal).clone();
        self.changed.notify_all();
        Some(terminal)
    }
    /// Whether the loader thread may be inside a package DLL's load now (its initializer runs under the loader
    /// lock): a load it committed has not returned (also one the deadline left running), or the run is still open, so a
    /// load may start any moment. False for a run concluded without the loader, and once the loader's last load
    /// returned after the run was concluded or finished. Never waits and never concludes the run.
    pub fn may_be_loading(&self) -> bool {
        let progress = self.lock();
        progress.loading.is_some() || !(progress.closed || progress.finished)
    }
    /// Results that arrived after their DLL had already timed out (diagnostics only).
    pub fn late_results(&self) -> Vec<(String, Outcome)> {
        self.lock().late.clone()
    }
    fn records(&self, progress: &Progress) -> Vec<TerminalRecord> {
        self.index
            .iter()
            .enumerate()
            .map(|(position, &(p, d))| {
                let package = &self.plan.packages[p];
                let dll = &package.dlls[d];
                TerminalRecord {
                    status_id: dll.status_id.clone(),
                    mod_id: package.mod_id.clone(),
                    package_directory: package.directory.clone(),
                    path: dll.path.clone(),
                    planned_sha256: dll.sha256.clone(),
                    actual_sha256: progress.actual[position].clone(),
                    mode: dll.mode,
                    source: dll.source.clone(),
                    outcome: progress.outcomes[position]
                        .clone()
                        .expect("every record is terminal"),
                }
            })
            .collect()
    }
    /// The packages that will not run. `refusal`: a reason that withdrew every package with a
    /// planned DLL before anything was attempted (the policy, or a run concluded without the loader).
    fn withdrawn(&self, records: &[TerminalRecord], refusal: Option<&str>) -> Vec<Withdrawn> {
        let mut withdrawn: BTreeMap<String, (String, Vec<String>)> = BTreeMap::new();
        for package in &self.plan.packages {
            if let Some(text) = refusal.filter(|_| !package.dlls.is_empty()) {
                withdrawn.insert(package.mod_id.clone(), (text.to_owned(), Vec::new()));
                continue;
            }
            if let Some(record) = records
                .iter()
                .find(|r| r.mod_id == package.mod_id && !r.outcome.admitted())
                && !matches!(record.outcome, Outcome::Withdrawn(_))
            {
                withdrawn.insert(
                    package.mod_id.clone(),
                    (
                        format!("native DLL {} did not load", record.path),
                        Vec::new(),
                    ),
                );
                continue;
            }
            if let Some(dep) = package
                .dependencies
                .iter()
                .find(|dep| withdrawn.contains_key(*dep))
            {
                let mut chain = vec![dep.clone()];
                chain.extend(withdrawn[dep].1.iter().cloned());
                withdrawn.insert(
                    package.mod_id.clone(),
                    (format!("depends on withdrawn package {dep}"), chain),
                );
            }
        }
        let order: BTreeMap<&str, usize> = self
            .plan
            .packages
            .iter()
            .enumerate()
            .map(|(i, p)| (p.mod_id.as_str(), i))
            .collect();
        let mut out: Vec<_> = withdrawn
            .into_iter()
            .map(|(mod_id, (reason, caused_by))| Withdrawn {
                mod_id,
                reason,
                caused_by,
            })
            .collect();
        out.sort_by_key(|w| order.get(w.mod_id.as_str()).copied());
        out
    }
}

/// The record line (kiln.log) for a DLL whose loader call returned after the deadline had
/// concluded the run (its record is `TimedOut`, its package withdrawn).
fn late_line(dll: &PlannedDll, outcome: &Outcome) -> String {
    let what = match outcome {
        Outcome::Loaded { .. } => "loaded: its code runs in the game process".to_owned(),
        Outcome::ForeignOwned { .. } => "another loader had already loaded it".to_owned(),
        Outcome::Refused(why) => format!("refused: {why}"),
        Outcome::Faulted(why) => format!("faulted: {why}"),
        Outcome::TimedOut(why) | Outcome::Withdrawn(why) => why.clone(),
    };
    format!(
        "native DLL '{}' ({}) returned from the loader after the native startup deadline had concluded this start's native run ({what}); the result is never admitted and its package stays withdrawn this start",
        dll.path, dll.status_id
    )
}

/// One DLL: re-establish, capture its package-local closure, gate, bind and load. `start` is
/// asked once, immediately before the loader call; `false` loads nothing.
fn load_one<B: Backend, S: Sink + Send + 'static>(
    enforcer: &mut Enforcer<B, S>,
    registry: &NativeContracts,
    root: &Path,
    dll: &PlannedDll,
    owner: u64,
    start: impl FnOnce() -> bool,
) -> (Outcome, Option<String>) {
    if dll.mode == Mode::Dinput8 {
        return (Outcome::Refused("DirectInput start needs Kiln's DirectInput8 provider (kiln.proxy), which this build does not have; the interpreter does not load it".into()), None);
    }
    if !["selection", "profile", "ini", "lua-registration"].contains(&dll.source.as_str()) {
        return (
            Outcome::Refused(format!(
                "{} entries are not the interpreter's to load (dinput8-chain.ini is Kiln's proxy route)",
                dll.source
            )),
            None,
        );
    }
    if dll.bridge_abi != 0 {
        return (
            Outcome::Refused(
                "the ShroudForge 1.8 native bridge is held (kiln-sfi.sf18 veto)".into(),
            ),
            None,
        );
    }
    let path = root.join(&dll.path);
    let entry = match Snapshot::capture(&path, &dll.path) {
        Ok(entry) => entry,
        Err(error) => return (Outcome::Refused(format!("{}: {error}", dll.path)), None),
    };
    let actual = Some(entry.digest().to_owned());
    // resolve_package_dll_path's containment rule, on the held, link-resolved path: a folder
    // link inside the package must not move the DLL (and its load folder) elsewhere. Under Wine
    // canonicalize keeps a Unix link in the path, so Wine says where the DLL really is.
    match std::fs::canonicalize(root) {
        Ok(folder)
            if entry.path().starts_with(&folder)
                && sfi_core::same_folder::wine_within(entry.path(), &folder).unwrap_or(false) => {}
        Ok(_) => {
            return (
                Outcome::Refused(format!(
                    "{}: DLL is outside its installed mod folder",
                    dll.path
                )),
                actual,
            );
        }
        Err(error) => return (Outcome::Refused(format!("{}: {error}", dll.path)), actual),
    }
    if entry.digest() != dll.sha256 {
        return (
            Outcome::Refused(format!(
                "{} changed since the launch plan pinned it (SHA-256 {} instead of {})",
                dll.path,
                entry.digest(),
                dll.sha256
            )),
            actual,
        );
    }
    let dependencies = match closure(&entry, &dll.path) {
        Ok(dependencies) => dependencies,
        Err(error) => return (Outcome::Refused(error), actual),
    };
    let permit = match enforcer.inspect(&entry, &dependencies, registry) {
        Ok(permit) => permit,
        Err(error) => return (Outcome::Refused(error), actual),
    };
    let outcome = match enforcer.load_if(owner, &permit, start) {
        Ok(LoadOutcome::Loaded(module)) => Outcome::Loaded { module },
        Ok(LoadOutcome::ForeignOwned(module)) => Outcome::ForeignOwned { module },
        Err(error) if enforcer.is_quarantined(owner) => Outcome::Faulted(error),
        Err(error) => Outcome::Refused(error),
    };
    (outcome, actual)
}

/// Snapshots of the package-local DLLs the entry's closure imports (ordinary, delay and
/// forwarded), from the entry's own folder only, as ShroudForge's load flags search. System and
/// API-set names are left to the OS resolver; an unparseable entry gets no dependencies, so
/// the permit reports its structural refusal.
fn closure(entry: &Snapshot, name: &str) -> Result<Vec<Snapshot>, String> {
    let folder = entry
        .path()
        .parent()
        .ok_or("native image has no folder")?
        .to_path_buf();
    let prefix = name.rsplit_once('/').map(|(dir, _)| format!("{dir}/"));
    let mut files: BTreeMap<String, String> = BTreeMap::new();
    for item in std::fs::read_dir(&folder).map_err(|e| e.to_string())? {
        let item = item.map_err(|e| e.to_string())?;
        if item.file_type().map_err(|e| e.to_string())?.is_file()
            && let Some(file) = item.file_name().to_str()
        {
            files.insert(file.to_ascii_lowercase(), file.to_owned());
        }
    }
    let own = name.rsplit('/').next().unwrap_or(name).to_ascii_lowercase();
    let mut seen = BTreeSet::from([own]);
    let mut pending = vec![entry.bytes().to_vec()];
    let mut out: Vec<Snapshot> = Vec::new();
    while let Some(bytes) = pending.pop() {
        let Ok(parsed) = pe_gate::parse(&bytes) else {
            continue;
        };
        for import in parsed.imports {
            let module = import.module.to_ascii_lowercase();
            let module = if module.contains('.') {
                module
            } else {
                format!("{module}.dll")
            };
            if module.starts_with("api-")
                || module.starts_with("ext-")
                || !seen.insert(module.clone())
            {
                continue;
            }
            let Some(file) = files.get(&module) else {
                continue;
            };
            if out.len() >= 1023 {
                return Err("package dependency closure exceeds 1024 images".into());
            }
            let logical = format!("{}{file}", prefix.as_deref().unwrap_or(""));
            let snapshot = Snapshot::capture(&folder.join(file), &logical)?;
            pending.push(snapshot.bytes().to_vec());
            out.push(snapshot);
        }
    }
    Ok(out)
}
