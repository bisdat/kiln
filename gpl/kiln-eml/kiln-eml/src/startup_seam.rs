// SPDX-License-Identifier: GPL-3.0-or-later
// kiln.eml.startup v1's provider (the interpreter's startup seam, the plugin wiring).
//! kiln-eml: kiln.eml.startup v1's provider (native/core/abi/services/kiln_eml_startup.h,
//! docs/kiln/services/eml-startup.md) and kiln-eml's side of a kiln-mode start, around the startup controller
//! (crate::startup):
//!
//! - [`early`] (kiln-eml's `kiln_plugin_early`, the boot main thread, before any package DLL loads): the frozen launch
//!   plan read as kiln-sfi reads it (kiln_mode.rs, sfi-policy's reader), the install lock taken for the process
//!   (`runner::held_lock`), the startup controller's [`startup::witness`] inside `catch_unwind` (a panic is a refused witness: fail closed,
//!   no grant), and the launch installed, so that kiln-sfi's early phase, which runs next, can attach.
//! - The table ([`kiln_eml_startup_service`], an early export only, never `register_service`): `attach` once with
//!   kiln.launch's key; `witness_wait` answers READY, REFUSED or PENDING; `witness_recheck` checks the stamped witness
//!   again (file metadata only); `native_terminal` takes the first terminal, checked against native-terminal.schema.json
//!   v1 and the plan; `outcome` serves the record and the identity once the barrier finished; `published_identity`
//!   (the boot identity, session-free) serves that identity alone, to kiln.boot, once the barrier finished with a grant.
//! - [`barrier`] (kiln-eml's early on_ready, the boot main thread): attach closes; after an attach it waits at most
//!   [`TERMINAL_WAIT`] from the attach for the terminal. The end of that wait and the decision on a terminal are one step
//!   under the lock, so a terminal or a recheck after it is UNAVAILABLE. Then the startup controller's [`startup::run`] (or the refused
//!   witness's outcome), the grant (`archive_service::authorize_with_archive`, PUBLISHED, once per start, never PREPARED,
//!   never retired in-process) and the outcome record with its identity.
//!
//! Threads and the loader lock: kiln-sfi calls `witness_wait` and `witness_recheck` from its loader thread and may call
//! `witness_recheck` and `native_terminal` from its deadline thread while a package's DllMain holds the loader lock
//! (the loader). No member loads a library, resolves a symbol, starts or joins a thread, logs, or initializes a lazy static
//! that could: they take one `Mutex` (never held across a wait: `witness_wait` and the barrier wait on a `Condvar`),
//! copy bytes, parse the terminal and, for the recheck, read file metadata (the stamped witness `InputWitness::check`). A terminal
//! refused after the wait, or a second one, is noted for kiln.log and logged at Kiln's plugin step ([`late_notes`]).
//! Every member is panic-contained (INTERNAL) and never enters Lua. In-process code is trusted: the session prevents
//! accidental misuse. The barrier's pass may run while that DllMain still holds the loader lock too: what it would ask
//! the system once per process through the loader (Wine's loader takes its lock for every LoadLibrary and
//! GetProcAddress, also of a module already loaded) is asked in the early phase instead ([`prime_the_pass`]).
use std::{
    collections::{BTreeMap, BTreeSet},
    ffi::c_char,
    fs::File,
    panic::{AssertUnwindSafe, catch_unwind},
    rc::Rc,
    sync::{
        Arc, Condvar, Mutex, MutexGuard,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, Instant},
};

use camino::{Utf8Path, Utf8PathBuf};
use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_UNAVAILABLE, KILN_OK, KilnResult,
    KilnServiceHeader, KilnStr,
    services::{
        kiln_eml_archive::{KILN_EML_ARCHIVE_PUBLISHED, KilnEmlArchiveIdentityV1},
        kiln_eml_startup::{
            KILN_EML_STARTUP_KEY_SIZE, KILN_EML_STARTUP_VERSION, KILN_EML_STARTUP_WITNESS_SIZE,
            KILN_EML_WITNESS_PENDING, KILN_EML_WITNESS_READY, KILN_EML_WITNESS_REFUSED, KilnEmlStartupApiV1,
            KilnEmlStartupKeyV1, KilnEmlStartupWitnessV1,
        },
    },
};
use mod_loader::ModEnvironment;
use sfi_core::{
    bindings::runtime::{Availability, RuntimeOperation},
    host::{Clock, DiskSource, LogLevel, LogSink, PackageSource, SourceVersion},
    package::{
        ParseOptions,
        discover::{PackageRecord, Registry, discover_source},
        profiles::ProfileCatalog,
    },
};
use sfi_policy::launch_plan::ValidationContext;

use crate::{
    archive_service::{self, Grant, Phase as ArchivePhase},
    kiln_mode::{self, Plan},
    runner::{Outcome, Phase},
    sf_startup::{plan::RuntimeCatalog, runner as pass_runner},
    startup::{
        self, Data, EmlSelection, LaunchKey, NativeFacts, PackageKind, PlanFacts, Point, Ports, Refusal, Refused,
        StartupOutcome, State as OutcomeState, Witnessed,
    },
};

/// How long the barrier waits, from kiln-sfi's attach, for its native terminal.
pub const TERMINAL_WAIT: Duration = Duration::from_secs(20);

/// The largest native terminal read (its schema allows 4096 withdrawals and 16384 DLL records).
const TERMINAL_MAX: usize = 64 << 20;

const KEY_SIZE: u32 = KILN_EML_STARTUP_KEY_SIZE;
const WITNESS_SIZE: u32 = KILN_EML_STARTUP_WITNESS_SIZE;
const IDENTITY_SIZE: u32 = std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;
const _: () = assert!(std::mem::size_of::<KilnEmlStartupKeyV1>() == 56);
const _: () = assert!(std::mem::size_of::<KilnEmlStartupWitnessV1>() == 40);
const _: () = assert!(std::mem::size_of::<KilnEmlStartupApiV1>() == 56);

/// The reason of every package of a kiln-mode start whose early phase witnessed nothing.
pub const NO_START: &str = "kiln-eml's early phase did not witness this start's game files (it did not run, or it \
    stopped), so no startup pass ran this start: its data was not applied, so the game is as without the mod. Start the \
    game again from the Kiln launcher";

// ---- the state ----

/// The early witness, as `witness_wait` answers it.
enum Witness {
    Pending,
    Ready(Arc<Witnessed>),
    Refused(String),
}

/// What the barrier learns of the native side.
#[derive(Clone, Debug)]
struct Terminal {
    coherent: Result<(), String>,
    withdrawn: BTreeMap<String, Refusal>,
    dlls: BTreeMap<String, Vec<String>>,
}

/// The one terminal of the start.
enum TerminalSlot {
    /// None arrived yet.
    Waiting,
    /// The first call counted: Some, a valid terminal kiln-eml's pass uses; None, a refused one (no terminal).
    Taken(Option<Terminal>),
}

/// kiln-eml's startup barrier.
enum BarrierState {
    NotBegun,
    Begun,
    /// The outcome every `outcome` call answers with: the identity (zero after its size without a grant), the record.
    Finished(Arc<(KilnEmlArchiveIdentityV1, String)>),
    /// It stopped before its outcome existed: `outcome` stays UNAVAILABLE for the process.
    Faulted,
}

/// The launch kiln-eml's early phase installed (a kiln-backend start whose plan was read again).
struct Launch {
    launch_id: u64,
    plan_sha256: [u8; 32],
    plan_id: [u8; 16],
    /// Every package id the plan lists (a terminal naming another is refused).
    listed: Arc<BTreeSet<String>>,
}

/// What the barrier needs of the early phase.
struct Start {
    plan: Plan,
    facts: PlanFacts,
    key: LaunchKey,
    game: Utf8PathBuf,
    stem: String,
    guard: Option<Arc<File>>,
    witness: Result<Arc<Witnessed>, Refused>,
    /// The install lock could not be taken: the existing stop before the game's entry point follows (K1).
    start_blocked: bool,
}

struct State {
    early_ran: bool,
    /// The plan the early phase read again, or why it could not (the kiln route takes it, no second read).
    plan: Option<Result<Plan, String>>,
    launch: Option<Launch>,
    witness: Witness,
    /// The session attach issued (0: none).
    session: u64,
    attached_at: Option<Instant>,
    planned_dlls: u32,
    /// The READY witness `witness_wait` gave this session (plan id, revision).
    issued: Option<([u8; 16], u64)>,
    terminal: TerminalSlot,
    /// The barrier's wait for the terminal ended: a later terminal or recheck is UNAVAILABLE.
    wait_ended: bool,
    barrier: BarrierState,
    start: Option<Start>,
    /// Lines for kiln.log that a member could not write itself (it may run under the loader lock).
    late: Vec<String>,
}

static STATE: Mutex<State> = Mutex::new(State {
    early_ran: false,
    plan: None,
    launch: None,
    witness: Witness::Pending,
    session: 0,
    attached_at: None,
    planned_dlls: 0,
    issued: None,
    terminal: TerminalSlot::Waiting,
    wait_ended: false,
    barrier: BarrierState::NotBegun,
    start: None,
    late: Vec::new(),
});
/// Signalled when the witness is decided or a terminal is taken.
static CHANGED: Condvar = Condvar::new();

fn state() -> MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// kiln-eml's process-unique generation numbers (nonzero).
static GENERATION: AtomicU64 = AtomicU64::new(0x6b65_6d6c_0000_0001);

// ---- the early phase ----

/// The startup controller's early witness, as [`EarlyInputs::witness`] takes it.
pub type WitnessFn = dyn Fn(&Utf8Path, &str, &File, &PlanFacts, LaunchKey) -> Result<Witnessed, Refused>;

/// What kiln-eml's plugin read from the core in its early step (plugin.rs), for [`early`].
pub struct EarlyInputs<'a> {
    pub game: &'a Utf8Path,
    pub kiln: &'a Utf8Path,
    /// The archive stem of the process's executable (`enshrouded`, `enshrouded_server`); anything else refuses the witness.
    pub stem: &'a str,
    /// kiln.launch's launch id and plan hash (backend `kiln`).
    pub launch_id: u64,
    pub plan_sha256: [u8; 32],
    /// `build_info`'s executable SHA-256 and side (`client` / `server`); None: the core does not say.
    pub build: Option<(String, &'static str)>,
    pub profile_json: Option<&'a str>,
    /// Another EML loader patched the game files in this process (plugin.rs's check): the witness is refused with it.
    pub foreign_loader: Option<String>,
    /// Takes the install lock for the process and lends it (production: [`install_lock`]).
    pub lock: &'a dyn Fn(&Utf8Path) -> std::io::Result<Arc<File>>,
    /// The startup controller's early witness (production: [`startup::witness`]); run inside `catch_unwind`.
    pub witness: &'a WitnessFn,
}

/// The production install lock: `runner::lock_game` (another game process of the install, or the lock busy, refuses),
/// lent from `runner::held_lock` (never dropped or taken again while the process lives).
pub fn install_lock(game: &Utf8Path) -> std::io::Result<Arc<File>> {
    crate::runner::lock_game(game)?;
    crate::runner::held_lock(game).ok_or_else(|| std::io::Error::other("the install lock taken is not held"))
}

/// kiln-eml's early phase of a kiln-backend start (module docs): the plan read again, the install lock, the startup controller's witness,
/// the launch installed for kiln-sfi's attach. Once per process. Returns the plan read (or why it could not be), which
/// the kiln route takes ([`early_plan`]). A panic is contained: the launch is not installed (attach UNAVAILABLE).
pub fn early(inputs: &EarlyInputs<'_>) -> Result<Plan, String> {
    match catch_unwind(AssertUnwindSafe(|| early_inner(inputs))) {
        Ok(read) => read,
        Err(panic) => {
            let why = format!(
                "kiln-eml's early phase stopped on an internal error: {}",
                panic_text(panic)
            );
            let mut st = state();
            st.launch = None;
            st.start = None;
            st.plan = Some(Err(why.clone()));
            Err(why)
        }
    }
}

fn early_inner(inputs: &EarlyInputs<'_>) -> Result<Plan, String> {
    // what the witness's checks ask the system, asked here on the boot thread (never later under the loader lock)
    mod_loader::lua::prime_witness_checks();
    prime_the_pass();
    {
        let mut st = state();
        if st.early_ran {
            return st
                .plan
                .clone()
                .unwrap_or_else(|| Err("kiln-eml's early phase ran already".into()));
        }
        st.early_ran = true;
    }
    let read = (|| {
        let (exe_sha256, side) = inputs
            .build
            .clone()
            .ok_or("the core does not say which game executable runs")?;
        let context = ValidationContext {
            exe_sha256,
            side: side.to_owned(),
            upstream_180_final: false,
        };
        kiln_mode::read_plan(inputs.profile_json, inputs.kiln, inputs.game, &inputs.plan_sha256, &context)
    })();
    let plan = match read {
        Ok(plan) => plan,
        Err(why) => {
            state().plan = Some(Err(why.clone()));
            return Err(why);
        }
    };
    let facts = plan.facts();
    let key = LaunchKey {
        launch_id: inputs.launch_id,
        plan_sha256: inputs.plan_sha256,
        plan_id: facts.plan_id,
    };
    {
        let mut st = state();
        st.plan = Some(Ok(plan.clone()));
        st.launch = Some(Launch {
            launch_id: inputs.launch_id,
            plan_sha256: inputs.plan_sha256,
            plan_id: facts.plan_id,
            listed: Arc::new(facts.packages.iter().map(|p| p.id.clone()).collect()),
        });
    }
    let refuse = |code: &str, text: String| Refused {
        code: code.into(),
        text,
    };
    let mut start_blocked = false;
    let mut guard = None;
    let witness = if let Some(why) = &inputs.foreign_loader {
        Err(refuse(
            "eml-loader-loaded",
            format!("{why}: the game files this start's launch plan was made for are not the ones the game reads now"),
        ))
    } else if !matches!(inputs.stem, "enshrouded" | "enshrouded_server") {
        Err(refuse(
            "invalid",
            format!("{} is not a game archive Kiln manages", inputs.stem),
        ))
    } else {
        match (inputs.lock)(inputs.game) {
            Err(e) => {
                start_blocked = true;
                Err(refuse(
                    "lock-busy",
                    format!(
                        "the game's install lock could not be taken ({e}); another process may still be publishing game data"
                    ),
                ))
            }
            Ok(lock) => {
                let witnessed = catch_unwind(AssertUnwindSafe(|| {
                    (inputs.witness)(inputs.game, inputs.stem, &lock, &facts, key)
                }))
                .unwrap_or_else(|panic| {
                    Err(refuse(
                        "startup-internal-error",
                        format!(
                            "kiln-eml's startup witness stopped on an internal error: {}",
                            panic_text(panic)
                        ),
                    ))
                });
                guard = Some(lock);
                witnessed
            }
        }
    };
    let witness = witness.map(Arc::new);
    {
        let mut st = state();
        st.witness = match &witness {
            Ok(w) => Witness::Ready(w.clone()),
            Err(refused) => Witness::Refused(refused.text.clone()),
        };
        st.start = Some(Start {
            plan: plan.clone(),
            facts,
            key,
            game: inputs.game.to_owned(),
            stem: inputs.stem.to_owned(),
            guard,
            witness,
            start_blocked,
        });
    }
    CHANGED.notify_all();
    Ok(plan)
}

/// What the barrier's pass asks the system once per process, asked here on the boot thread before any package DLL
/// loads: the pass may run while a package's DllMain still holds the loader lock, and Wine's loader takes that lock for
/// every LoadLibrary and GetProcAddress, also of a module already loaded, and for RtlPcToFileHeader (Proton stalled the
/// pass until the DllMain returned; native Windows does not). plugin.rs asks for on_ready's own (the executable's name,
/// Wine's path conversion) in the same early phase.
fn prime_the_pass() {
    // the first Lua state of the process: Lua's seed asks the C runtime for the time, whose first call resolves
    // GetSystemTimePreciseAsFileTime with LoadLibraryExW("api-ms-win-core-sysinfo-l1-2-1")
    drop(mlua::Lua::new());
    // The first thread-local with a destructor: std then sets up its thread-local destructors' callback
    // (std::sys::thread_local::guard::windows: FlsAlloc, and GetModuleHandleExW from the callback's address, which
    // Wine answers through RtlPcToFileHeader under the loader lock; the pass's first such thread-local waited for a
    // package DLL's initializer there)
    thread_local!(static PRIMED: std::cell::RefCell<Vec<u8>> = const { std::cell::RefCell::new(Vec::new()) });
    PRIMED.with(|primed| primed.borrow_mut().push(0));
    // the containment checks of a package's folder and files under Wine (sfi-package's same_folder, which sfi-core's
    // package source and rules reader use, and mod-loader-base's wine_within: Wine's Unix name of a path, whose export
    // each looks up once through GetModuleHandleW and GetProcAddress)
    let _ = sfi_core::same_folder::under_wine();
    mod_loader::prime_wine_within();
    // sfi-core's export-root checks (eml io secure.rs), which make the same lookup on their own (exports are off
    // in kiln mode today; asked here so that no lazy loader lookup is left on the pass's path when they are not)
    let _ = sfi_core::eml::io::prime_export_checks();
}

/// The plan kiln-eml's early phase read again (None: the early phase did not run in this process).
pub fn early_plan() -> Option<Result<Plan, String>> {
    state().plan.clone()
}

/// Lines a member noted for kiln.log (a terminal kiln-eml refused after its wait), taken once: plugin.rs logs them at
/// Kiln's plugin step, on the boot thread.
pub fn late_notes() -> Vec<String> {
    std::mem::take(&mut state().late)
}

// ---- the barrier ----

/// What the barrier needs of the run (runner.rs `run_kiln`).
pub struct BarrierPorts<'a> {
    /// kiln-eml's cache (`<game>/kiln/eml/cache`): the type registry's cache and EML's change cache.
    pub cache_dir: &'a Utf8Path,
    /// The unlisted EML contributors the EML pass runs in a start without a startup dialect (kiln_mode's Q1 rule).
    pub unlisted: &'a [Utf8PathBuf],
    /// How long to wait for the terminal after the attach ([`TERMINAL_WAIT`] in production).
    pub wait: Duration,
    /// The startup controller's points (tests' kill points; nothing in production).
    pub observe: &'a dyn Fn(Point<'_>),
}

/// A finished barrier.
pub struct Finished {
    pub outcome: StartupOutcome,
    /// The PUBLISHED identity granted, when there is a grant.
    pub identity: Option<KilnEmlArchiveIdentityV1>,
    /// The record `outcome` serves.
    pub json: String,
    /// How long the barrier waited for the terminal.
    pub waited: Duration,
    /// The install lock could not be taken (K1).
    pub start_blocked: bool,
}

/// What [`barrier`] did.
pub enum Barriered {
    /// No start: kiln-eml's early phase installed none (not a kiln start, the plan could not be read, it stopped), or the
    /// barrier ran already.
    NoStart,
    /// It stopped before its outcome existed (`outcome` stays UNAVAILABLE).
    Faulted(String),
    Done(Box<Finished>),
}

/// kiln-eml's startup barrier (module docs). Once per process, on the boot main thread.
pub fn barrier(ports: &BarrierPorts<'_>) -> Barriered {
    let began = Instant::now();
    let (start, native) = {
        let mut st = state();
        if !matches!(st.barrier, BarrierState::NotBegun) {
            return Barriered::NoStart;
        }
        st.barrier = BarrierState::Begun;
        let Some(start) = st.start.take() else {
            return Barriered::NoStart;
        };
        let attached = st.session != 0;
        if let (true, Some(at)) = (attached, st.attached_at) {
            let deadline = at + ports.wait;
            while matches!(st.terminal, TerminalSlot::Waiting) {
                let now = Instant::now();
                if now >= deadline {
                    break;
                }
                st = CHANGED
                    .wait_timeout(st, deadline - now)
                    .unwrap_or_else(|poisoned| poisoned.into_inner())
                    .0;
            }
        }
        // the end of the wait: from here a terminal or a recheck is UNAVAILABLE
        st.wait_ended = true;
        let terminal = match &st.terminal {
            TerminalSlot::Taken(Some(t)) => Some(t.clone()),
            _ => None,
        };
        let native = NativeFacts {
            attached,
            received: terminal.is_some(),
            withdrawn: terminal.as_ref().map(|t| t.withdrawn.clone()).unwrap_or_default(),
            coherent: terminal.as_ref().map_or(Ok(()), |t| t.coherent.clone()),
            dlls: terminal.map(|t| t.dlls).unwrap_or_default(),
        };
        (start, native)
    };
    let waited = began.elapsed();
    let blocked = start.start_blocked;
    match catch_unwind(AssertUnwindSafe(|| finish(start, &native, ports))) {
        Ok((outcome, identity, json)) => {
            let served = identity.unwrap_or_else(|| {
                // SAFETY: integers and byte arrays: all-zero is a valid record.
                let mut zero: KilnEmlArchiveIdentityV1 = unsafe { std::mem::zeroed() };
                zero.size = IDENTITY_SIZE;
                zero
            });
            state().barrier = BarrierState::Finished(Arc::new((served, json.clone())));
            CHANGED.notify_all();
            Barriered::Done(Box::new(Finished {
                outcome,
                identity,
                json,
                waited,
                start_blocked: blocked,
            }))
        }
        Err(panic) => {
            state().barrier = BarrierState::Faulted;
            Barriered::Faulted(format!(
                "kiln-eml's startup barrier stopped on an internal error: {}",
                panic_text(panic)
            ))
        }
    }
}

/// The barrier after its wait: the pass (or the refused witness's outcome), the grant, the record.
fn finish(
    start: Start,
    native: &NativeFacts,
    ports: &BarrierPorts<'_>,
) -> (StartupOutcome, Option<KilnEmlArchiveIdentityV1>, String) {
    let Start {
        plan,
        facts,
        key,
        game,
        stem,
        guard,
        witness,
        ..
    } = start;
    let mut outcome = match (&witness, &guard) {
        (Err(refused), _) => startup::refused(&facts, key, native, refused),
        (Ok(w), Some(guard)) => pass(w, &plan, &facts, native, guard, &game, &stem, ports),
        (Ok(_), None) => {
            let refused = Refused {
                code: "lock-busy".into(),
                text: "the game's install lock is not held by this start".into(),
            };
            let mut out = startup::refused(&facts, key, native, &refused);
            out.state = OutcomeState::Failed;
            out
        }
    };
    let identity = if outcome.state.grants() {
        match grant(&outcome, key) {
            Ok(identity) => Some(identity),
            Err(why) => {
                let text = format!("kiln-eml could not grant Kiln's interpreter the live archive generation ({why})");
                crate::sf_pass::log_warning(&text);
                outcome = startup::without_grant(outcome, &facts, "startup-grant-missing", &text);
                None
            }
        }
    } else {
        None
    };
    changed_input_by_dll(&outcome);
    let json = outcome.json(identity.as_ref());
    (outcome, identity, json)
}

/// The chosen rule, kiln-eml's part in this one place:
/// a package DLL changed the game's archive files during startup (the terminal's recheck or kiln-eml's own check before
/// its pass, `run` step 1). The start continues, with no ShroudForge startup data written and no grant (so no
/// interpreter), and kiln.log gets the loud line naming the DLL packages that loaded. No stop of the start.
fn changed_input_by_dll(outcome: &StartupOutcome) {
    if let Some(warning) = outcome.warnings.iter().find(|w| w.code == "startup-input-changed") {
        tracing::error!(target: "startup-assets", "{}", warning.text);
    }
}

/// The startup controller's pass over the witness with the production ports.
#[allow(clippy::too_many_arguments)]
fn pass(
    w: &Witnessed,
    plan: &Plan,
    facts: &PlanFacts,
    native: &NativeFacts,
    guard: &File,
    game: &Utf8Path,
    stem: &str,
    ports: &BarrierPorts<'_>,
) -> StartupOutcome {
    let types = match mod_loader::lua::load_type_registry(game, ports.cache_dir, stem) {
        Ok((types, _)) => Rc::new(types),
        Err(e) => {
            let refused = Refused {
                code: "startup-types-unavailable".into(),
                text: format!("Kiln could not read the game's reflection types for the startup pass: {e:#}"),
            };
            let mut out = startup::refused(facts, w.key, native, &refused);
            out.state = OutcomeState::Failed;
            return out;
        }
    };
    let catalog = Rc::new(PregameCatalog {
        game_build: kfc::container::KFCFile::get_version_tag(game.join(format!("{stem}.kfc.bak")))
            .unwrap_or_else(|_| "unknown".into()),
    });
    let eml = |selection: EmlSelection<'_>| -> anyhow::Result<Option<ModEnvironment>> {
        let paths: Vec<Utf8PathBuf> = match selection {
            EmlSelection::Admitted(ids) => ids
                .iter()
                .filter_map(|id| facts.package(id))
                .filter(|p| p.kind == PackageKind::Eml)
                .map(|p| game.join(&p.root))
                .collect(),
            EmlSelection::Unlisted => ports.unlisted.to_vec(),
        };
        if paths.is_empty() {
            return Ok(None);
        }
        match ModEnvironment::load_from(game, ports.cache_dir, paths) {
            Ok(env) => Ok(Some(env)),
            Err(report) => {
                let mut why: Vec<String> = report.error.iter().map(|e| e.to_string()).collect();
                why.extend(report.mods.iter().map(|m| format!("{}: {}", m.path, m.error)));
                anyhow::bail!("{}", why.join("; "))
            }
        }
    };
    let digest = |root: &std::path::Path| sfi_policy::digest::package_content_sha256(root);
    let log = |line: &str| crate::sf_pass::log_warning(line);
    startup::run(
        w,
        facts,
        native,
        guard,
        Ports {
            types,
            registry: pass_registry(game, plan),
            catalog,
            // owner question Q-C: the start's UI locale is not in the plan yet; None answers "en", as ShroudForge does
            locale: None,
            vm: pass_runner::Ports {
                clock: Rc::new(ProcessClock(process_origin())),
                log: Rc::new(PackageLog),
            },
            digest: &digest,
            eml: &eml,
            log: &log,
            observe: ports.observe,
        },
    )
}

/// The packages below `<game>/mods` as kiln-sfi discovers them (plan_source.rs `discover`): ShroudForge's discovery with
/// no profile, each package switched on exactly as the plan has it. The ShroudForge pass's registry and
/// `loader.has_mod`'s membership; never an admission.
fn pass_registry(game: &Utf8Path, plan: &Plan) -> Registry {
    let mods = game.join("mods").into_std_path_buf();
    let options = ParseOptions {
        version: SourceVersion::Sf179,
        ..ParseOptions::default()
    };
    let report = DiskSource::new(&mods)
        .and_then(|root| {
            discover_source(
                &root,
                |entry| Ok(Rc::new(DiskSource::new(mods.join(&entry.path))?) as Rc<dyn PackageSource>),
                options,
                &ProfileCatalog::default(),
                &[],
            )
        })
        .unwrap_or_default();
    let launch = plan.launch_plan();
    let mut registry = Registry::default();
    for record in report.registry.values() {
        let mut parsed = record.parsed().clone();
        parsed.manifest.enabled = launch.package(&parsed.manifest.id).is_some_and(|p| p.enabled);
        if let Ok(record) = PackageRecord::new(
            record.directory().to_owned(),
            parsed,
            record.runtime().clone(),
            record.raw_source().clone(),
        ) {
            let _ = registry.insert(record);
        }
    }
    registry
}

/// The pregame VM's runtime operation catalog (the runtime registrar's static support), a decision of the plugin wiring:
/// kiln-eml has no verified operation contract of its own for the selected build (kiln-sfi's catalog decides in game,
/// with the core's services), so before the game every operation is unverified for the archive's game version, with
/// ShroudForge's text. Never a fabricated "everything available".
struct PregameCatalog {
    game_build: String,
}
impl RuntimeCatalog for PregameCatalog {
    fn game_build(&self) -> &str {
        &self.game_build
    }
    fn static_support(&self, operation: RuntimeOperation) -> Availability {
        Availability::Unavailable {
            reason: format!(
                "operation {} has not been verified for game build {}",
                operation.as_str(),
                self.game_build
            ),
        }
    }
}

/// The process's monotonic origin (one per process, as kiln-sfi's `process_clock`).
fn process_origin() -> Instant {
    static ORIGIN: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
    *ORIGIN.get_or_init(Instant::now)
}
struct ProcessClock(Instant);
impl Clock for ProcessClock {
    fn seconds(&self) -> f64 {
        self.0.elapsed().as_secs_f64()
    }
}

/// The packages' log lines in the startup pass: kiln.log and kiln/logs/eml.log under target `startup-assets`, tagged
/// with the package; ShroudForge's default minimum level INFO (as kiln-sfi's mod log).
struct PackageLog;
impl LogSink for PackageLog {
    fn enabled(&self, level: LogLevel) -> bool {
        !matches!(level, LogLevel::Trace | LogLevel::Debug)
    }
    fn write(&self, package: &str, level: LogLevel, payload: &[u8]) {
        let text = String::from_utf8_lossy(payload);
        match level {
            LogLevel::Error => tracing::error!(target: "startup-assets", "[{package}] {text}"),
            LogLevel::Warn => tracing::warn!(target: "startup-assets", "[{package}] {text}"),
            _ => tracing::info!(target: "startup-assets", "[{package}] {text}"),
        }
    }
}

/// The grant of the generation the outcome proves live: PUBLISHED, once per start, with this launch's pins (
/// section 2): launch_id is 8 zero bytes and kiln.launch's u64 big-endian, reload_epoch 1, a process-unique generation.
fn grant(outcome: &StartupOutcome, key: LaunchKey) -> Result<KilnEmlArchiveIdentityV1, String> {
    let live = outcome.live.as_ref().ok_or("the live generation is not described")?;
    let mut launch_id = [0u8; 16];
    launch_id[8..].copy_from_slice(&key.launch_id.to_be_bytes());
    archive_service::authorize_with_archive(
        Grant {
            phase: ArchivePhase::Published,
            launch_id,
            plan_id: key.plan_id,
            generation: GENERATION.fetch_add(1, Ordering::Relaxed),
            reload_epoch: 1,
            plan_sha256: key.plan_sha256,
            archive_sha256: live.archive_sha256,
            prepared_owners: Vec::new(),
        },
        live.snapshot.clone(),
        live.registry.clone(),
        live.backing.clone(),
    )
}

// ---- the state file ----

/// kiln/state/eml.json of a kiln-mode start from its barrier's outcome (runner.rs `run_kiln`): one row per package of the
/// pass, `source: "shroudforge"` (the discovered entry of the same package becomes it), the unlisted contributors of a
/// start without a startup dialect as the EML pass left them, the phase from the outcome's state.
pub fn settle_entries(out: &mut Outcome, finished: &Finished, plan: &Plan, game: &Utf8Path, unlisted: &[Utf8PathBuf]) {
    let outcome = &finished.outcome;
    let facts = plan.facts();
    for row in &outcome.packages {
        let (state, reason) = match row.data {
            Data::Committed | Data::CachedVerified => ("loaded", None),
            Data::Failed => ("failed", row.text.clone()),
            Data::Excluded => (
                "skipped",
                Some(match (&row.text, &row.code) {
                    (Some(text), Some(code)) => format!("{text} ({code})"),
                    (Some(text), None) => text.clone(),
                    (None, code) => code.clone().unwrap_or_default(),
                }),
            ),
        };
        let root = facts.package(&row.id).map(|p| game.join(&p.root));
        // the same folder, whatever separators its path was written with (kiln_mode's own rule)
        let at_root = |path: &str| {
            root.as_ref().is_some_and(|r| {
                crate::discover::relative_under(Utf8Path::new(path), r).is_some_and(|rel| rel.as_str().is_empty())
            })
        };
        let at = out
            .mods
            .iter()
            .position(|e| e.id.as_deref() == Some(row.id.as_str()) || at_root(&e.path));
        let entry = match at {
            Some(i) => &mut out.mods[i],
            None => {
                out.mods.push(crate::discover::Entry {
                    id: None,
                    version: None,
                    name: None,
                    source: String::new(),
                    path: root.map(|r| r.to_string()).unwrap_or_default(),
                    state: String::new(),
                    reason: None,
                    capabilities: Vec::new(),
                });
                out.mods.last_mut().expect("just pushed")
            }
        };
        entry.id = Some(row.id.clone());
        if row.version.is_some() {
            entry.version = row.version.clone();
        }
        entry.source = "shroudforge".into();
        entry.state = state.into();
        entry.reason = reason;
    }
    let grants = outcome.state.grants();
    let why = outcome
        .warnings
        .first()
        .map(|w| w.text.clone())
        .or_else(|| outcome.error.clone())
        .unwrap_or_else(|| "kiln-eml's startup pass granted nothing this start".into());
    // the unlisted contributors the EML pass of a start without a startup dialect ran; any other entry still pending
    // (a package of the pass the outcome has no row for) did not run
    for e in out.mods.iter_mut().filter(|e| e.state == "pending") {
        if !unlisted.iter().any(|u| u.as_str() == e.path) {
            e.state = "skipped".into();
            e.reason = Some(format!("kiln-eml's startup outcome has no row for it ({why})"));
            continue;
        }
        match outcome.state {
            OutcomeState::Committed | OutcomeState::Cached => e.state = "loaded".into(),
            _ if grants => {
                e.state = "skipped".into();
                e.reason = Some("the EML pass of this start applied nothing".into());
            }
            _ => {
                e.state = "failed".into();
                e.reason = Some(why.clone());
            }
        }
    }
    out.phase = match outcome.state {
        OutcomeState::Committed | OutcomeState::Cached => Phase::Done,
        OutcomeState::Unchanged | OutcomeState::Restored => Phase::Idle,
        OutcomeState::Refused | OutcomeState::Failed => Phase::Failed,
    };
    out.cached = outcome.state == OutcomeState::Cached;
    out.restored = outcome.state == OutcomeState::Restored;
    out.error = (!grants).then(|| why.clone());
    out.start_blocked = finished.start_blocked;
}

// ---- the table ----

static TABLE: KilnEmlStartupApiV1 = KilnEmlStartupApiV1 {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnEmlStartupApiV1>() as u32,
        version: KILN_EML_STARTUP_VERSION,
    },
    attach: Some(attach),
    witness_wait: Some(witness_wait),
    witness_recheck: Some(witness_recheck),
    native_terminal: Some(native_terminal),
    outcome: Some(outcome),
    published_identity: Some(published_identity),
};

/// kiln.eml.startup v1's early getter (kiln_eml_startup.h): any thread, side-effect free, the one process-lifetime table
/// for `min_version` 0 and 1, NULL above. It grants nothing: every member checks the key.
#[unsafe(no_mangle)]
pub extern "C" fn kiln_eml_startup_service(min_version: u32) -> *const KilnEmlStartupApiV1 {
    if min_version <= KILN_EML_STARTUP_VERSION {
        &TABLE
    } else {
        std::ptr::null()
    }
}

/// A panic never crosses the C boundary: it is INTERNAL.
fn contained(work: impl FnOnce() -> KilnResult) -> KilnResult {
    catch_unwind(AssertUnwindSafe(work)).unwrap_or(KILN_ERR_INTERNAL)
}

fn panic_text(panic: Box<dyn std::any::Any + Send>) -> String {
    panic
        .downcast_ref::<&str>()
        .map(|s| s.to_string())
        .or_else(|| panic.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "a panic".into())
}

/// A v1 key (its size first, zero flags), copied.
///
/// # Safety
/// `key` is null or readable for its declared size.
unsafe fn read_key(key: *const KilnEmlStartupKeyV1) -> Result<KilnEmlStartupKeyV1, KilnResult> {
    if key.is_null() {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    }
    // SAFETY: the caller's record, its size first.
    let size = unsafe { std::ptr::addr_of!((*key).size).read_unaligned() };
    if size < KEY_SIZE {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    }
    // SAFETY: as above; the size covers v1.
    let key = unsafe { key.read_unaligned() };
    if key.flags != 0 {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    }
    Ok(key)
}

/// The key names this start's launch and the session attach issued.
fn this_session(st: &State, key: &KilnEmlStartupKeyV1) -> Result<(), KilnResult> {
    match &st.launch {
        Some(l)
            if st.session != 0
                && key.session == st.session
                && key.launch_id == l.launch_id
                && key.plan_sha256 == l.plan_sha256 =>
        {
            Ok(())
        }
        _ => Err(KILN_ERR_INVALID_ARGUMENT),
    }
}

/// Counted text into the caller's buffer: `required` the complete size, then BUFFER_TOO_SMALL (nothing else written) or
/// the bytes and OK.
///
/// # Safety
/// `required` is writable; `buffer` holds `capacity` bytes (or is null with capacity 0).
unsafe fn write_text(text: &[u8], buffer: *mut c_char, capacity: usize, required: *mut usize) -> KilnResult {
    // SAFETY: the caller's contract.
    unsafe { required.write_unaligned(text.len()) };
    if capacity < text.len() {
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    if !text.is_empty() {
        // SAFETY: capacity >= len bytes.
        unsafe { std::ptr::copy_nonoverlapping(text.as_ptr(), buffer.cast::<u8>(), text.len()) };
    }
    KILN_OK
}

/// The v1 fields of a filled witness record (its size kept, flags and reserved zero).
///
/// # Safety
/// `out` is writable for the v1 record.
unsafe fn fill_witness(out: *mut KilnEmlStartupWitnessV1, state: i32, plan_id: [u8; 16], revision: u64) {
    // SAFETY: the caller's contract; the size the caller set is kept.
    unsafe {
        let size = std::ptr::addr_of!((*out).size).read_unaligned();
        out.write_unaligned(KilnEmlStartupWitnessV1 {
            size,
            state: state as u32,
            plan_id,
            revision,
            flags: 0,
            reserved: 0,
        });
    }
}

unsafe extern "C" fn attach(key: *const KilnEmlStartupKeyV1, planned_dlls: u32, session: *mut u64) -> KilnResult {
    contained(|| {
        // SAFETY: the caller's record.
        let key = match unsafe { read_key(key) } {
            Ok(key) => key,
            Err(e) => return e,
        };
        if session.is_null() || key.session != 0 || key.launch_id == 0 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let mut st = state();
        let Some(launch) = &st.launch else {
            // not a kiln-backend start of this kiln-eml, or its early phase failed
            return KILN_ERR_UNAVAILABLE;
        };
        if key.launch_id != launch.launch_id || key.plan_sha256 != launch.plan_sha256 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        if st.session != 0 || !matches!(st.barrier, BarrierState::NotBegun) {
            return KILN_ERR_UNAVAILABLE;
        }
        let issued = (launch.launch_id ^ 0x6b69_6c6e_5345_4d31).rotate_left(13) | 1;
        st.session = issued;
        st.attached_at = Some(Instant::now());
        st.planned_dlls = planned_dlls;
        // SAFETY: the caller's out-pointer.
        unsafe { session.write_unaligned(issued) };
        KILN_OK
    })
}

unsafe extern "C" fn witness_wait(
    key: *const KilnEmlStartupKeyV1,
    timeout_ms: u32,
    out: *mut KilnEmlStartupWitnessV1,
    reason: *mut c_char,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    contained(|| {
        // SAFETY: the caller's record.
        let key = match unsafe { read_key(key) } {
            Ok(key) => key,
            Err(e) => return e,
        };
        if out.is_null() || required.is_null() || (reason.is_null() && capacity != 0) {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // SAFETY: the caller's record, its size first.
        if unsafe { std::ptr::addr_of!((*out).size).read_unaligned() } < WITNESS_SIZE {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let deadline = Instant::now() + Duration::from_millis(u64::from(timeout_ms));
        let mut st = state();
        if let Err(e) = this_session(&st, &key) {
            return e;
        }
        while matches!(st.witness, Witness::Pending) {
            let now = Instant::now();
            if now >= deadline {
                break;
            }
            st = CHANGED
                .wait_timeout(st, deadline - now)
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .0;
        }
        match &st.witness {
            Witness::Ready(w) => {
                let (plan_id, revision) = (w.key.plan_id, w.revision);
                st.issued = Some((plan_id, revision));
                // SAFETY: checked above.
                unsafe {
                    fill_witness(out, KILN_EML_WITNESS_READY, plan_id, revision);
                    required.write_unaligned(0);
                }
                KILN_OK
            }
            Witness::Refused(text) => {
                let text = text.clone();
                drop(st);
                // SAFETY: checked above.
                let rc = unsafe { write_text(text.as_bytes(), reason, capacity, required) };
                if rc == KILN_OK {
                    // SAFETY: checked above.
                    unsafe { fill_witness(out, KILN_EML_WITNESS_REFUSED, [0; 16], 0) };
                }
                rc
            }
            Witness::Pending => {
                // SAFETY: checked above.
                unsafe {
                    fill_witness(out, KILN_EML_WITNESS_PENDING, [0; 16], 0);
                    required.write_unaligned(0);
                }
                KILN_OK
            }
        }
    })
}

unsafe extern "C" fn witness_recheck(
    key: *const KilnEmlStartupKeyV1,
    witness: *const KilnEmlStartupWitnessV1,
    reason: *mut c_char,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    contained(|| {
        // SAFETY: the caller's record.
        let key = match unsafe { read_key(key) } {
            Ok(key) => key,
            Err(e) => return e,
        };
        if witness.is_null() || required.is_null() || (reason.is_null() && capacity != 0) {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // SAFETY: the caller's record, its size first.
        if unsafe { std::ptr::addr_of!((*witness).size).read_unaligned() } < WITNESS_SIZE {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // SAFETY: as above; the size covers v1.
        let w = unsafe { witness.read_unaligned() };
        let witnessed = {
            let st = state();
            if let Err(e) = this_session(&st, &key) {
                return e;
            }
            if w.flags != 0
                || w.reserved != 0
                || w.state != KILN_EML_WITNESS_READY as u32
                || st.issued != Some((w.plan_id, w.revision))
            {
                return KILN_ERR_INVALID_ARGUMENT;
            }
            if st.wait_ended {
                return KILN_ERR_UNAVAILABLE;
            }
            match &st.witness {
                Witness::Ready(witnessed) => witnessed.clone(),
                _ => return KILN_ERR_INVALID_ARGUMENT,
            }
        };
        // metadata and the small parsed files only: no lock held, nothing loaded
        match witnessed.input.check(witnessed.game()) {
            Ok(()) => {
                // SAFETY: checked above.
                unsafe { required.write_unaligned(0) };
                KILN_OK
            }
            Err(e) => {
                let text = format!("the game's archive files changed after kiln-eml witnessed them: {e}");
                // SAFETY: checked above.
                unsafe { write_text(text.as_bytes(), reason, capacity, required) }
            }
        }
    })
}

unsafe extern "C" fn native_terminal(key: *const KilnEmlStartupKeyV1, json: KilnStr) -> KilnResult {
    contained(|| {
        // SAFETY: the caller's record.
        let key = match unsafe { read_key(key) } {
            Ok(key) => key,
            Err(e) => return e,
        };
        if json.ptr.is_null() && json.len != 0 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let (plan_id, listed, planned) = {
            let st = state();
            if let Err(e) = this_session(&st, &key) {
                return e;
            }
            let launch = st.launch.as_ref().expect("a session has its launch");
            (launch.plan_id, launch.listed.clone(), st.planned_dlls)
        };
        let bytes: &[u8] = if json.len == 0 {
            &[]
        } else {
            // SAFETY: counted bytes for the call.
            unsafe { std::slice::from_raw_parts(json.ptr.cast::<u8>(), json.len) }
        };
        // read before the lock: the decision below is one step with the barrier's wait
        let read = read_terminal(bytes, plan_id, &listed, planned);
        let mut st = state();
        if !matches!(st.terminal, TerminalSlot::Waiting) {
            st.late.push(format!(
                "kiln-eml: refused a second native terminal from kiln-sfi ({} bytes); the first one counts",
                bytes.len()
            ));
            return KILN_ERR_UNAVAILABLE;
        }
        if st.wait_ended {
            st.late.push(format!(
                "kiln-eml: kiln-sfi's native terminal arrived after kiln-eml's startup wait ended; its startup pass ran \
                 without it ({} bytes)",
                bytes.len()
            ));
            return KILN_ERR_UNAVAILABLE;
        }
        let rc = if read.is_ok() {
            KILN_OK
        } else {
            KILN_ERR_INVALID_ARGUMENT
        };
        if let Err(why) = &read {
            st.late.push(format!(
                "kiln-eml: refused kiln-sfi's native terminal ({why}); the DLL packages do not start"
            ));
        }
        st.terminal = TerminalSlot::Taken(read.ok());
        drop(st);
        CHANGED.notify_all();
        rc
    })
}

unsafe extern "C" fn outcome(
    key: *const KilnEmlStartupKeyV1,
    identity: *mut KilnEmlArchiveIdentityV1,
    json: *mut c_char,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    contained(|| {
        // SAFETY: the caller's record.
        let key = match unsafe { read_key(key) } {
            Ok(key) => key,
            Err(e) => return e,
        };
        if identity.is_null() || required.is_null() || (json.is_null() && capacity != 0) {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // SAFETY: the caller's record, its size first.
        let size = unsafe { std::ptr::addr_of!((*identity).size).read_unaligned() };
        if size < IDENTITY_SIZE {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let finished = {
            let st = state();
            if let Err(e) = this_session(&st, &key) {
                return e;
            }
            match &st.barrier {
                BarrierState::Finished(finished) => finished.clone(),
                _ => return KILN_ERR_UNAVAILABLE,
            }
        };
        let (served, text) = &*finished;
        // SAFETY: checked above.
        let rc = unsafe { write_text(text.as_bytes(), json, capacity, required) };
        if rc == KILN_OK {
            let mut copy = *served;
            copy.size = size;
            // SAFETY: the caller's record, at least the v1 size; bytes past v1 untouched.
            unsafe { identity.write_unaligned(copy) };
        }
        rc
    })
}

/// The identity alone of the generation this start granted, for kiln.boot's OnBoot game data (kiln_eml_startup.h
/// published_identity). Session-free and read-only: it takes no session, changes no state and grants nothing. OK only
/// once the barrier finished with a grant, with exactly the identity `outcome` serves; UNAVAILABLE before that, for a
/// start without a grant (or a barrier that faulted) and in a process with no launch installed (not a kiln-backend start
/// of this kiln-eml, or its early phase failed). A key that names another launch or plan, a session, a short record or a
/// null pointer is INVALID_ARGUMENT. Nothing is written on a failure; one lock taken, bytes copied, nothing logged.
unsafe extern "C" fn published_identity(
    key: *const KilnEmlStartupKeyV1,
    identity: *mut KilnEmlArchiveIdentityV1,
) -> KilnResult {
    contained(|| {
        // SAFETY: the caller's record.
        let key = match unsafe { read_key(key) } {
            Ok(key) => key,
            Err(e) => return e,
        };
        if identity.is_null() || key.session != 0 || key.launch_id == 0 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // SAFETY: the caller's record, its size first.
        let size = unsafe { std::ptr::addr_of!((*identity).size).read_unaligned() };
        if size < IDENTITY_SIZE {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let served = {
            let st = state();
            let Some(launch) = &st.launch else {
                return KILN_ERR_UNAVAILABLE;
            };
            if key.launch_id != launch.launch_id || key.plan_sha256 != launch.plan_sha256 {
                return KILN_ERR_INVALID_ARGUMENT;
            }
            match &st.barrier {
                BarrierState::Finished(finished) if finished.0.phase == KILN_EML_ARCHIVE_PUBLISHED as u32 => finished.0,
                _ => return KILN_ERR_UNAVAILABLE,
            }
        };
        let mut copy = served;
        copy.size = size;
        // SAFETY: the caller's record, at least the v1 size; bytes past v1 untouched.
        unsafe { identity.write_unaligned(copy) };
        KILN_OK
    })
}

// ---- the native terminal (native-terminal.schema.json v1) ----

/// A ShroudForge identifier (sfi-state's sfId).
fn sf_id(text: &str) -> bool {
    (1..=80).contains(&text.len())
        && text
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
}
/// sfi-state's code grammar.
fn code_word(text: &str) -> bool {
    (1..=64).contains(&text.len())
        && text
            .bytes()
            .next()
            .is_some_and(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
        && text
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'.' || b == b'-')
}
fn uuid(b: &[u8; 16]) -> String {
    let h: String = b.iter().map(|x| format!("{x:02x}")).collect();
    format!("{}-{}-{}-{}-{}", &h[..8], &h[8..12], &h[12..16], &h[16..20], &h[20..])
}
/// `dll:<folder>:<path>`, lower case, no control character.
fn status_id(text: &str) -> bool {
    let Some(rest) = text.strip_prefix("dll:") else {
        return false;
    };
    let Some((folder, path)) = rest.split_once(':') else {
        return false;
    };
    let plain = |s: &str| {
        !s.is_empty()
            && !s
                .chars()
                .any(|c| c.is_ascii_uppercase() || ('\u{0}'..='\u{1f}').contains(&c))
    };
    text.chars().count() <= 65536 && plain(folder) && !folder.contains(':') && plain(path)
}
fn closed<'a>(
    value: &'a serde_json::Value,
    what: &str,
    keys: &[&str],
    optional: &[&str],
) -> Result<&'a serde_json::Map<String, serde_json::Value>, String> {
    let map = value.as_object().ok_or_else(|| format!("{what} is not an object"))?;
    if let Some(k) = map
        .keys()
        .find(|k| !keys.contains(&k.as_str()) && !optional.contains(&k.as_str()))
    {
        return Err(format!("{what} has an unknown member {k:?}"));
    }
    if let Some(k) = keys.iter().find(|k| !map.contains_key(**k)) {
        return Err(format!("{what} lacks {k}"));
    }
    Ok(map)
}
fn text_of(value: &serde_json::Value, what: &str) -> Result<String, String> {
    match value.as_str() {
        Some(t) if !t.is_empty() && t.chars().count() <= 4096 => Ok(t.to_owned()),
        _ => Err(format!("{what} is not a text of 1 to 4096 characters")),
    }
}

/// The check `native_terminal` makes of a terminal (for tests: native-terminal.schema.json's fixtures): Ok when kiln-eml's
/// pass would use it.
pub fn check_native_terminal(
    bytes: &[u8],
    plan_id: [u8; 16],
    listed: &BTreeSet<String>,
    planned: u32,
) -> Result<(), String> {
    read_terminal(bytes, plan_id, listed, planned).map(|_| ())
}

/// The native terminal kiln-eml's pass uses, read strictly: native-terminal.schema.json v1 (closed objects, its bounds and
/// rules), the plan's id, attach's `planned_dlls` DLL records, only packages the plan lists, and every package with a DLL
/// that did not load withdrawn. A withdrawal of a package with no DLL record is accepted (a Lua-only dependent, a
/// package that only registers DLLs from its Lua, or a native plan kiln-sfi could not read: it attaches with 0 DLLs).
fn read_terminal(bytes: &[u8], plan_id: [u8; 16], listed: &BTreeSet<String>, planned: u32) -> Result<Terminal, String> {
    if bytes.len() > TERMINAL_MAX {
        return Err(format!("it is larger than {TERMINAL_MAX} bytes"));
    }
    let doc: serde_json::Value = serde_json::from_slice(bytes).map_err(|e| format!("it does not parse: {e}"))?;
    let top = closed(
        &doc,
        "the terminal",
        &[
            "format",
            "planId",
            "notArmed",
            "deadlinePassed",
            "coherent",
            "withdrawn",
            "dlls",
        ],
        &[],
    )?;
    if top["format"].as_u64() != Some(1) {
        return Err("format is not 1".into());
    }
    if top["planId"].as_str() != Some(uuid(&plan_id).as_str()) {
        return Err("planId is not this start's plan".into());
    }
    let not_armed = top["notArmed"].as_bool().ok_or("notArmed is not a boolean")?;
    let deadline = top["deadlinePassed"]
        .as_bool()
        .ok_or("deadlinePassed is not a boolean")?;
    let coherent = closed(&top["coherent"], "coherent", &["ok"], &["text"])?;
    let coherent = match (coherent["ok"].as_bool(), coherent.get("text")) {
        (Some(true), None) => Ok(()),
        (Some(false), Some(text)) => Err(text_of(text, "coherent.text")?),
        _ => return Err("coherent is not {ok: true} or {ok: false, text}".into()),
    };
    let withdrawn_list = top["withdrawn"].as_array().ok_or("withdrawn is not an array")?;
    if withdrawn_list.len() > 4096 {
        return Err("more than 4096 withdrawals".into());
    }
    let mut withdrawn = BTreeMap::new();
    for item in withdrawn_list {
        let w = closed(item, "a withdrawal", &["id", "code", "text", "chain"], &[])?;
        let id = w["id"]
            .as_str()
            .filter(|id| sf_id(id))
            .ok_or("a withdrawal's id is not a ShroudForge identifier")?;
        if !listed.contains(id) {
            return Err(format!("it withdraws {id}, which the plan does not list"));
        }
        let code = w["code"]
            .as_str()
            .filter(|c| code_word(c))
            .ok_or("a withdrawal's code is not a code word")?;
        let text = text_of(&w["text"], "a withdrawal's text")?;
        let chain: Vec<String> = w["chain"]
            .as_array()
            .ok_or("a withdrawal's chain is not an array")?
            .iter()
            .map(|c| c.as_str().filter(|c| sf_id(c)).map(str::to_owned))
            .collect::<Option<_>>()
            .ok_or("a chain entry is not a ShroudForge identifier")?;
        if chain.len() > 64 || chain.iter().any(|c| c == id) {
            return Err(format!("{id}'s chain is longer than 64 or names itself"));
        }
        let refusal = Refusal {
            code: code.to_owned(),
            text,
            chain,
        };
        if withdrawn.insert(id.to_owned(), refusal).is_some() {
            return Err(format!("{id} is withdrawn twice"));
        }
    }
    let records = top["dlls"].as_array().ok_or("dlls is not an array")?;
    if records.len() > 16384 || records.len() != planned as usize {
        return Err(format!("it has {} DLL records, attach said {planned}", records.len()));
    }
    let mut dlls: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for item in records {
        let d = closed(item, "a DLL record", &["statusId", "modId", "outcome"], &[])?;
        if !d["statusId"].as_str().is_some_and(status_id) {
            return Err("a DLL record's statusId is not dll:<folder>:<path> in lower case".into());
        }
        let id = d["modId"]
            .as_str()
            .filter(|id| sf_id(id))
            .ok_or("a DLL record's modId is not a ShroudForge identifier")?;
        if !listed.contains(id) {
            return Err(format!("it has a DLL of {id}, which the plan does not list"));
        }
        let outcome = d["outcome"]
            .as_str()
            .filter(|o| {
                [
                    "loaded",
                    "foreign-owned",
                    "refused",
                    "faulted",
                    "timed-out",
                    "withdrawn",
                ]
                .contains(o)
            })
            .ok_or("a DLL record's outcome is not one the native loader writes")?;
        if not_armed && outcome != "refused" {
            return Err("a run that was not armed has a DLL that was not refused".into());
        }
        if !deadline && outcome == "timed-out" {
            return Err("a DLL timed out while the deadline did not pass".into());
        }
        if !matches!(outcome, "loaded" | "foreign-owned") && !withdrawn.contains_key(id) {
            return Err(format!("a DLL of {id} did not load, and {id} is not withdrawn"));
        }
        dlls.entry(id.to_owned()).or_default().push(outcome.to_owned());
    }
    if not_armed && (deadline || coherent.is_err()) {
        return Err("a run that was not armed passed the deadline or is not coherent".into());
    }
    Ok(Terminal {
        coherent,
        withdrawn,
        dlls,
    })
}
