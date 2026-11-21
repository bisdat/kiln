// SPDX-License-Identifier: MIT
//! MIT plugin boundary for Kiln's interpreter of ShroudForge mods.
//!
//! Which ShroudForge backend has a start is the core's frozen kiln.launch record
//! never the plugins' file order and never launch.json.
//! Both boot phases read it ([`read_launch`]) and succeed quietly, preserve
//! separate owners and publish a truthful unavailable capability:
//! - `kiln`: Kiln's interpreter has this start. The plugin step's on_ready starts
//!   it, unless ShroudForge's own loader is in this process (then kiln-sfi stands
//!   aside, as kiln-shroudforge does). That loader is looked for in the early phase,
//!   before any package DLL loads, and at main and once more right before the start, since
//!   it can arrive in between, unless kiln-sfi's own native run may still be inside a
//!   package DLL's load there ([`Boundary::look_for_own_loader`]). A boundary built
//!   with [`Boundary::with_worker`] starts the interpreter worker there, as one
//!   kiln.tasks task under the Normal-phase owner. The exported boundary is
//!   one, bound to the production worker ([`plan_source::KilnPlanSource`]). A
//!   boundary without a worker refuses the start and says why.
//!
//!   Such a boundary reads the frozen launch plan once ([`plan_source`]): in the early
//!   phase, or at main if the early phase never ran; the worker gets that read, never
//!   the file. A plan the core accepted but kiln-sfi's strict reader refuses (or one
//!   that changed since the freeze) is a `refused` start with a loud warning, never a
//!   hosted one.
//! - hosted, installed, or a refusal: kiln-sfi stands aside and starts nothing.
//! - the idle class: the core decided nothing; kiln-sfi does nothing.
//! - no table (an older core) or one that cannot be read: the rule from before the
//!   table (nothing starts).
//!
//! Under `kiln`, Kiln's own kiln.json `native.policy` gates ShroudForge package DLLs as it gates
//! Kiln's native mods: the record carries the frozen policy and whether the build is
//! verified (`build_verified`). Right after the plan read the early phase makes the one native
//! manager run of the process ([`native_manager::arming`]) from them: its loader is armed only
//! when the run was made in the early phase, the policy allows, the plan lists a DLL and the startup
//! witness is wired, otherwise every planned DLL is refused at once with no notifier and no
//! thread. The plugin step reads `KilnApi.build_info().verified` again
//! ([`Boundary::native_permission`]): when it disagrees with the record, the interpreter is refused
//! (`verified-disagrees`). When the policy refuses, no package DLL loads and kiln.log says so.
//!
//! **The startup handshake with kiln-eml** ([`startup_seam`], kiln.eml.startup v1). In the early
//! phase, right after the plan read: sfi.json's `starting` record is written first (package DLLs load
//! in this window and kiln-eml commits startup data in its early on_ready, both before Kiln's plugin
//! step; if that write fails nothing is armed or attached and on_ready refuses the start,
//! `state-unwritable`); then kiln-eml's early export is resolved and attached with kiln.launch's key and
//! the native plan's DLL count, which wires the native run's witness to it
//! ([`startup_seam::SeamWitness`]); the run's terminal goes back to kiln-eml once it is known, at
//! ShroudForge's 15 s startup deadline (counted from the arming) at the latest, while a hung package DLL's
//! initializer still runs ([`native_manager::arming::arm_and_push`]). At Kiln's
//! plugin step kiln-eml's startup outcome is read once and cross-checked; only a start it grants an
//! archive generation runs ([`plan_source::grant_archive`]), and the worker admits by the outcome's
//! verdict. Without a grant (no seam, no attach, no outcome, a mismatch, or a startup pass that proved
//! no live generation) the worker's runtime-create is refused with the loud warning and a `refused`
//! record, before any Lua. A start refused after kiln-eml applied its data says so per package in
//! sfi.json (`data-applied-runtime-stopped`).
//!
//! Nothing here enters Lua, loads a mod DLL, or changes game/profile files.
//! Fault-boundary primitives are tested on their own.
//!
//! kiln-sfi's own state file, `kiln/state/sfi.json` ([`state`]), says what it
//! did: `aside` or `refused` from the plugin's main, `starting` from the early phase
//! under `kiln` (at on_ready when the early phase never ran) before anything is armed,
//! attached or submitted (no start without it), `running` once the worker has its
//! runtime, `ended` from on_unload.
//!
//! Under `kiln` (after on_ready, or at main for a start refused there) kiln-sfi's own
//! do-not-save banner ([`notice_native`]) watches that record on kiln-sfi's IO
//! thread: it draws through kiln.ui only while a warning is pending and Kiln.Ui is absent,
//! and on_unload takes it down before the final flush.

pub mod fault;
pub mod hop;
pub mod input;
pub mod lifecycle;
pub mod native_enforcement;
pub mod native_manager;
pub mod notice_native;
pub mod plan_source;
pub mod runtime;
pub mod startup_seam;
pub mod state;
// The pure policy moved to sfi-policy; these are the same items under the old paths.
pub use sfi_policy::{launch_plan, native_contracts, pe_gate};
pub mod worker;

use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_launch::*;
use kiln_abi::*;
use native_manager::arming::{self, Armed, LoaderBody, TerminalPush, WitnessPort};
use native_manager::executor::{NativeRun, Witness};
use native_manager::plan::NativePlan;
use native_manager::selection::{NativePermission, NativePolicy};
use startup_seam::{Seam, SeamWitness, StartupRead};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Mutex, OnceLock};

type LogFn = unsafe extern "C" fn(KilnOwner, i32, KilnStr);
type ServiceFn = unsafe extern "C" fn(KilnStr, u32) -> *const core::ffi::c_void;

/// The core's frozen ShroudForge backend for this start (kiln.launch v1,
/// native/core/abi/services/kiln_launch.h), as kiln-sfi reads it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Launch {
    /// No kiln.launch (an older core), or a table or record that cannot be read.
    Absent,
    /// Backend none, a reason of the idle class: the core did not decide.
    Idle { code: u32, reason: String },
    /// Backend none, a reason of the refused class: every ShroudForge backend
    /// stands aside this start.
    Refused { code: u32, reason: String },
    /// Hosted or installed: kiln-shroudforge has this start.
    Shroudforge {
        installed: bool,
        code: u32,
        reason: String,
    },
    /// Kiln's interpreter has this start. `native_policy`: kiln.json native.policy as the core
    /// froze it (it gates ShroudForge package DLLs in kiln mode).
    Kiln {
        code: u32,
        reason: String,
        native_policy: NativePolicy,
    },
}

/// Reads the frozen record through `service`. Every table member is checked
/// against the table's declared size and version before use, and the record
/// against its own; anything else is [`Launch::Absent`]. kiln-sfi asks for the
/// whole record (200 bytes, native_policy included): a core that fills only the
/// first 192 never decided native_policy, so it is read as no table, and so is a
/// `kiln` record whose native_policy this build does not know.
///
/// # Safety
/// `service` is the core's `KilnApi.service`; a table it returns is readable for
/// its declared size.
pub unsafe fn read_launch(service: ServiceFn) -> Launch {
    unsafe { read_launch_record(service) }.launch
}

/// The frozen record with the identity fields sfi.json copies (launch id, plan
/// hash, freeze stamp). Those are set only for a record kiln-sfi can classify:
/// for [`Launch::Absent`] there is no launch id.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LaunchRecord {
    pub launch: Launch,
    /// kiln.launch's launch id; `None` with [`Launch::Absent`].
    pub launch_id: Option<u64>,
    pub backend: Option<u32>,
    pub reason_code: u32,
    pub reason: String,
    pub plan_hash: [u8; 32],
    pub freeze_stamp: u64,
    /// The record's `build_verified`: whether this game build is verified, as the boot
    /// answered before the freeze. `None` with [`Launch::Absent`] and for a value other than 0 or 1
    /// (which counts as not verified, as an answer kiln-sfi cannot get does).
    pub build_verified: Option<bool>,
}
impl LaunchRecord {
    const ABSENT: Self = Self {
        launch: Launch::Absent,
        launch_id: None,
        backend: None,
        reason_code: 0,
        reason: String::new(),
        plan_hash: [0; 32],
        freeze_stamp: 0,
        build_verified: None,
    };
}

/// [`read_launch`] with the record's identity fields.
///
/// # Safety
/// As [`read_launch`].
pub unsafe fn read_launch_record(service: ServiceFn) -> LaunchRecord {
    let name = KILN_LAUNCH_SERVICE
        .strip_suffix(b"\0")
        .unwrap_or(KILN_LAUNCH_SERVICE);
    let name = KilnStr {
        ptr: name.as_ptr().cast(),
        len: name.len(),
    };
    let table = unsafe { service(name, KILN_LAUNCH_VERSION) }.cast::<KilnLaunchApi>();
    if table.is_null() || !table.is_aligned() {
        return LaunchRecord::ABSENT;
    }
    // SAFETY: the table starts with its header; members are read only when the
    // declared size covers them.
    let header = unsafe { table.cast::<KilnServiceHeader>().read() };
    if header.version < KILN_LAUNCH_VERSION
        || (header.size as usize)
            < core::mem::offset_of!(KilnLaunchApi, status) + core::mem::size_of::<usize>()
    {
        return LaunchRecord::ABSENT;
    }
    let Some(status) = (unsafe { core::ptr::addr_of!((*table).status).read() }) else {
        return LaunchRecord::ABSENT;
    };
    // SAFETY: an all-zero record is valid; the core fills it during the call.
    let mut record: KilnLaunchStatus = unsafe { core::mem::zeroed() };
    record.size = core::mem::size_of::<KilnLaunchStatus>() as u32;
    if unsafe { status(&mut record) } != KILN_OK
        || record.frozen != 1
        || (record.size as usize) < core::mem::size_of::<KilnLaunchStatus>()
    {
        return LaunchRecord::ABSENT;
    }
    let bytes: Vec<u8> = record
        .reason
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    let reason = String::from_utf8_lossy(&bytes).into_owned();
    let code = record.reason_code;
    let launch = match record.backend as i32 {
        KILN_LAUNCH_BACKEND_HOSTED | KILN_LAUNCH_BACKEND_INSTALLED => Launch::Shroudforge {
            installed: record.backend as i32 == KILN_LAUNCH_BACKEND_INSTALLED,
            code,
            reason: reason.clone(),
        },
        KILN_LAUNCH_BACKEND_KILN => match NativePolicy::from_launch(record.native_policy) {
            Some(native_policy) => Launch::Kiln {
                code,
                reason: reason.clone(),
                native_policy,
            },
            None => return LaunchRecord::ABSENT,
        },
        KILN_LAUNCH_BACKEND_NONE if (0x100..0x200).contains(&code) => Launch::Refused {
            code,
            reason: reason.clone(),
        },
        KILN_LAUNCH_BACKEND_NONE if (0x200..0x300).contains(&code) => Launch::Idle {
            code,
            reason: reason.clone(),
        },
        _ => return LaunchRecord::ABSENT,
    };
    LaunchRecord {
        launch,
        launch_id: Some(record.launch_id),
        backend: Some(record.backend),
        reason_code: code,
        reason,
        plan_hash: record.plan_hash,
        freeze_stamp: record.freeze_stamp,
        build_verified: match record.build_verified {
            0 => Some(false),
            1 => Some(true),
            _ => None,
        },
    }
}

/// `KilnApi.build_info().verified` (kiln_api.h): this game build is in the
/// verified-builds list. The core serves it only from Kiln's plugin step on (the early
/// plugins get 0), so it is read there, never in the early phase: the early phase has the
/// record's `build_verified`, and the plugin step checks the two agree. `None`: the
/// core's table has no such member, or the call fails.
///
/// # Safety
/// `api` is NULL or a `KilnApi` readable for its declared `size`.
pub unsafe fn read_build_verified(api: *const KilnApi) -> Option<bool> {
    if api.is_null() || !api.is_aligned() {
        return None;
    }
    // SAFETY: the table starts with its size; the member is read only when it is covered.
    let size = unsafe { api.cast::<u32>().read() } as usize;
    if size < core::mem::offset_of!(KilnApi, build_info) + core::mem::size_of::<usize>() {
        return None;
    }
    let build_info = unsafe { core::ptr::addr_of!((*api).build_info).read() }?;
    // SAFETY: an all-zero record is valid; the core fills it during the call.
    let mut info: KilnBuildInfo = unsafe { core::mem::zeroed() };
    info.size = core::mem::size_of::<KilnBuildInfo>() as u32;
    (unsafe { build_info(&mut info) } == KILN_OK).then_some(info.verified != 0)
}

/// Kiln's native.policy for this start's ShroudForge package DLLs: the frozen policy
/// and whether the build is verified (unknown counts as not verified).
pub fn native_permission(policy: NativePolicy, verified: Option<bool>) -> NativePermission {
    NativePermission {
        policy,
        verified: verified == Some(true),
    }
}

/// kiln.log's warning when the native policy refuses ShroudForge package DLLs.
fn native_refusal_text(refusal: &str) -> String {
    format!(
        "kiln-sfi: {refusal}: no ShroudForge package DLL loads this start; every package that ships one is withdrawn, with every package that depends on it"
    )
}

/// "verified" or "not verified", for the disagreement's text.
fn verified_word(verified: bool) -> &'static str {
    if verified { "verified" } else { "not verified" }
}

/// What the native manager's wiring uses. [`NativeHooks::PRODUCTION`] in every boundary
/// unless a test replaces it ([`Boundary::with_native`]).
#[derive(Clone, Copy)]
pub struct NativeHooks {
    /// The startup witness port when no kiln-eml startup seam was attached: [`arming::not_wired`] in production (the
    /// boundary wires the seam's own witness when it attached one).
    pub witness: fn() -> WitnessPort,
    /// Starts the loader thread: [`arming::spawn_native_thread`].
    pub spawn: fn(LoaderBody) -> Result<(), String>,
    /// Starts an armed run's deadline waiter, before the loader thread: [`arming::spawn_deadline_thread`].
    pub waiter: fn(LoaderBody) -> Result<(), String>,
    /// ShroudForge's startup deadline for an armed run, counted from the arming:
    /// [`native_manager::executor::STARTUP_NATIVE_DEADLINE`] (15 s).
    pub deadline: std::time::Duration,
    /// The loader thread's body for a run: the Windows backend, kiln.log for the enforcement's notices
    /// (`log` and `owner`: kiln-sfi's early phase), the shipped contract registry.
    pub loader: fn(Arc<NativeRun>, LogFn, KilnOwner) -> LoaderBody,
}
impl NativeHooks {
    pub const PRODUCTION: Self = Self {
        witness: arming::not_wired,
        spawn: arming::spawn_native_thread,
        waiter: arming::spawn_deadline_thread,
        deadline: native_manager::executor::STARTUP_NATIVE_DEADLINE,
        loader: production_loader,
    };
}

/// The native terminal back to kiln-eml's startup seam (native-terminal.schema.json v1), once; kiln.log says when
/// kiln-eml did not take it. `log` and `owner`: kiln-sfi's early phase (any thread).
fn terminal_push(
    seam: Arc<Seam>,
    witness: Arc<SeamWitness>,
    plan_id: [u8; 16],
    permission: NativePermission,
    footprint: startup_seam::NativeFootprint,
    log: LogFn,
    owner: KilnOwner,
) -> TerminalPush {
    let policy = permission.refusal().map(str::to_owned);
    Box::new(move |terminal| {
        // a run never armed withdraws every package admission refuses for native code, not only those with a planned DLL
        let mut terminal = terminal.clone();
        terminal.withdrawn = footprint.withdrawn(&terminal);
        let json =
            startup_seam::terminal_json(plan_id, &terminal, policy.as_deref(), witness.refused());
        let (level, text) = match seam.push_terminal(&json) {
            KILN_OK => return,
            KILN_ERR_UNAVAILABLE => (
                KILN_LOG_INFO,
                "kiln-sfi: kiln-eml no longer takes the native terminal (its wait for it had ended); its startup pass runs without it".to_owned(),
            ),
            other => (
                KILN_LOG_WARN,
                format!(
                    "kiln-sfi: kiln-eml did not take the native terminal ({}); the packages that ship a DLL and their dependents get no startup data",
                    startup_seam::result_word(other)
                ),
            ),
        };
        // SAFETY: the core's log member, any thread.
        unsafe { log(owner, level, kstr(&text)) };
    })
}

/// The production loader: the Windows backend of the native loader; no other platform loads a ShroudForge package DLL.
fn production_loader(run: Arc<NativeRun>, log: LogFn, owner: KilnOwner) -> LoaderBody {
    #[cfg(windows)]
    {
        arming::loader_body(run, move || {
            Ok((
                native_enforcement::WindowsBackend::default(),
                LogSink { log, owner },
                native_contracts::NativeContracts::shipped()?,
            ))
        })
    }
    #[cfg(not(windows))]
    {
        let _ = (log, owner);
        Box::new(move || {
            run.abandon(
                "ShroudForge package DLLs load only on Windows: no ShroudForge package DLL loads this start",
            )
        })
    }
}

/// The native enforcement's notices as kiln.log lines of kiln-sfi's early owner (any thread).
#[cfg(windows)]
struct LogSink {
    log: LogFn,
    owner: KilnOwner,
}
#[cfg(windows)]
impl LogSink {
    fn line(&self, level: i32, text: &str) {
        let text = format!("kiln-sfi: {text}");
        // SAFETY: the core's log member, any thread.
        unsafe { (self.log)(self.owner, level, kstr(&text)) };
    }
}
#[cfg(windows)]
impl native_enforcement::Sink for LogSink {
    fn self_hosted(&mut self, reason: &str) {
        self.line(KILN_LOG_WARN, reason);
    }
    fn quarantine(&mut self, _: Option<u64>, reason: &str) {
        self.line(KILN_LOG_ERROR, reason);
    }
    fn record(&mut self, reason: &str) {
        self.line(KILN_LOG_WARN, reason);
    }
    fn save_warning(&mut self, reason: &str) {
        self.line(KILN_LOG_ERROR, &format!("WARNING: {reason}"));
    }
}

/// What kiln-sfi does this start.
#[derive(Clone, Debug, PartialEq, Eq)]
enum Selection {
    /// Kiln's interpreter has this start (the record's reason): the plugin step's
    /// on_ready starts it.
    Interpreter(String),
    /// kiln-sfi starts nothing. The text is the capability row's reason.
    Inactive(String),
}

/// The decision from the frozen record. `env_off`: `KILN_SHROUDFORGE=0`, which
/// stands every ShroudForge backend aside, Kiln's interpreter included, with that
/// one reason (for every record the core decided). `audit`: the early module
/// audit's finding (ShroudForge's runtime in this process, or the audit failing);
/// `own_loader`: ShroudForge's own loader found in this process. Both only matter
/// for `kiln`: kiln-shroudforge stands aside then, so a ShroudForge runtime here
/// is ShroudForge's own, whatever the plugins' file order.
fn select(
    launch: &Launch,
    env_off: bool,
    audit: Option<&str>,
    own_loader: Option<&str>,
    fallback: InactiveReason,
) -> Selection {
    let decided = !matches!(launch, Launch::Absent | Launch::Idle { .. });
    if env_off && decided {
        return Selection::Inactive(ENV_OFF.into());
    }
    match launch {
        Launch::Absent => {
            Selection::Inactive(audit.map_or_else(|| fallback.text().into(), str::to_owned))
        }
        Launch::Idle { reason, .. } => Selection::Inactive(format!(
            "the core decided no ShroudForge backend for this start ({reason}); kiln-sfi does nothing"
        )),
        Launch::Refused { reason, .. } => Selection::Inactive(format!(
            "every ShroudForge backend stands aside this start ({reason})"
        )),
        Launch::Shroudforge {
            installed, reason, ..
        } => Selection::Inactive(format!(
            "backend {}: kiln-shroudforge has this start ({reason})",
            if *installed { "installed" } else { "hosted" }
        )),
        Launch::Kiln { reason, .. } => match audit.or(own_loader) {
            Some(why) => Selection::Inactive(stands_aside(reason, why)),
            None => Selection::Interpreter(reason.clone()),
        },
    }
}

/// Why kiln-sfi stands aside under `kiln`: ShroudForge runs in this process itself.
/// The same words at main and, for a loader that arrived after main, at on_ready.
fn stands_aside(reason: &str, why: &str) -> String {
    format!("Kiln's interpreter has this start ({reason}), but {why}: kiln-sfi stands aside")
}

/// The capability row and log line when the interpreter starts at on_ready.
fn starts_text(reason: &str, worker: bool) -> String {
    if worker {
        format!(
            "Kiln's interpreter has this start ({reason}); its worker starts at Kiln's plugin step"
        )
    } else {
        format!(
            "Kiln's interpreter has this start ({reason}); this kiln-sfi build has no interpreter worker yet, so no ShroudForge mod runs"
        )
    }
}

/// kiln-shroudforge's switch, which turns every ShroudForge backend off.
const ENV_OFF: &str = "KILN_SHROUDFORGE=0";

/// `KILN_SHROUDFORGE=0` in this process (only the exact value 0, as
/// kiln-shroudforge reads it).
fn env_off() -> bool {
    std::env::var_os("KILN_SHROUDFORGE").is_some_and(|v| v == "0")
}

/// ShroudForge's own loader in this process: a module exporting `ShroudforgeStop`
/// (its bootstrap), as kiln-shroudforge's `sf_in_process` finds it.
#[cfg(windows)]
fn own_loader_in_process() -> Option<String> {
    use core::ffi::c_void;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentProcess() -> *mut c_void;
        fn K32EnumProcessModules(
            process: *mut c_void,
            modules: *mut *mut c_void,
            bytes: u32,
            needed: *mut u32,
        ) -> i32;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
        fn GetModuleFileNameW(module: *mut c_void, name: *mut u16, size: u32) -> u32;
    }
    let mut modules = vec![core::ptr::null_mut::<c_void>(); 1024];
    let mut needed = 0u32;
    let bytes = (modules.len() * core::mem::size_of::<*mut c_void>()) as u32;
    // SAFETY: the buffer holds `bytes` bytes; a module that unloads meanwhile only
    // makes GetProcAddress fail.
    if unsafe {
        K32EnumProcessModules(
            GetCurrentProcess(),
            modules.as_mut_ptr(),
            bytes,
            &mut needed,
        )
    } == 0
    {
        return None;
    }
    let count = (needed as usize / core::mem::size_of::<*mut c_void>()).min(modules.len());
    for &module in &modules[..count] {
        if unsafe { GetProcAddress(module, c"ShroudforgeStop".as_ptr().cast()) }.is_null() {
            continue;
        }
        let mut path = vec![0u16; 32768];
        let len =
            unsafe { GetModuleFileNameW(module, path.as_mut_ptr(), path.len() as u32) } as usize;
        let file = if len == 0 || len >= path.len() {
            "<loaded module path unavailable>".to_owned()
        } else {
            String::from_utf16_lossy(&path[..len])
        };
        return Some(format!("ShroudForge's own loader ({file}) runs it"));
    }
    None
}
/// No other platform has ShroudForge's loader.
#[cfg(not(windows))]
fn own_loader_in_process() -> Option<String> {
    None
}

pub const ID: &str = "kiln-sfi";
pub const CAPABILITY: &str = "kiln-sfi.interpreter";

#[derive(Clone, Copy)]
pub enum Phase {
    Early,
    Normal,
}

#[derive(Clone, Copy)]
pub enum InactiveReason {
    NegotiationMissing,
    Hosted,
    Installed,
}
impl InactiveReason {
    fn text(self) -> &'static str {
        match self {
            Self::NegotiationMissing => "backend negotiation not integrated",
            Self::Hosted => "backend hosted",
            Self::Installed => "backend installed",
        }
    }
}

struct State {
    api: usize,
    log: Option<LogFn>,
    service: Option<ServiceFn>,
    /// The record's reason, once the Normal phase installed on_ready for `kiln`.
    ready: Option<String>,
    /// Kiln's native.policy for package DLLs, read with `ready` (the Normal phase, `kiln`).
    native: Option<NativePermission>,
    owners: [Option<KilnOwner>; 2],
    reported: [bool; 2],
    logged: bool,
    observed_reason: Option<String>,
    /// The start's sfi.json identity, kept from main for on_ready (backend `kiln`).
    identity: Option<state::Identity>,
    /// What the early phase's look for ShroudForge's own loader found (`kiln`; None: it did not look).
    early_loader: Option<Option<String>>,
    #[cfg(any(windows, test))]
    early_scanned: bool,
}

/// Captures boot identity only. Admission/freeze tokens belong to the launch-plan
/// owner; this type intentionally offers no implicit activation/fallback method.
pub struct Boundary {
    state: Mutex<State>,
    reason: InactiveReason,
    worker: Option<&'static worker::Worker>,
    /// The one read of this start's frozen launch plan (a boundary with a worker, `kiln`).
    plan: plan_source::PlanCell,
    own_loader: fn() -> Option<String>,
    /// sfi.json's record and writer; shared (not borrowed) with the writer thread and
    /// the worker, so neither outlives it.
    status: std::sync::OnceLock<std::sync::Arc<state::StateHub>>,
    /// The native manager's one run, made right after the plan read (`kiln`, with a worker).
    native_run: OnceLock<Arc<NativeRun>>,
    native: NativeHooks,
    /// Where kiln-eml's early export is found: [`startup_seam::kiln_eml_export`] unless a test replaces it.
    startup: startup_seam::Resolve,
    /// kiln-eml's startup seam, resolved and attached once in the early phase.
    seam: OnceLock<SeamState>,
    /// kiln-eml's startup outcome, read once at Kiln's plugin step.
    outcome: OnceLock<Arc<StartupRead>>,
    /// The early phase could not write this start's sfi.json record: nothing was armed or attached, and on_ready
    /// refuses the start.
    early_unwritable: OnceLock<state::WriteError>,
    /// This start's native do-not-save banner (`kiln` only), made and started once.
    notice: OnceLock<Arc<notice_native::Notice>>,
    /// What the banner's probe and durations are: [`notice_native::NoticeHooks::PRODUCTION`] unless a test replaces it.
    notice_hooks: notice_native::NoticeHooks,
}

/// kiln-eml's startup seam as the early phase found it.
enum SeamState {
    /// No export: an older kiln-eml (or none).
    NotBuilt,
    /// An export that could not be used or refused the attach: sfi.json's code and why.
    Refused(&'static str, String),
    Attached(Arc<Seam>),
}

/// Why a start without an attach from the early phase has no startup outcome.
const ATTACH_MISSING: &str = "kiln-sfi's early phase did not attach to kiln-eml's startup seam (it did not run), so kiln-eml's startup outcome cannot be read";
impl Default for Boundary {
    fn default() -> Self {
        Self::new()
    }
}
impl Boundary {
    pub const fn new() -> Self {
        Self::inactive(InactiveReason::NegotiationMissing)
    }
    pub const fn inactive(reason: InactiveReason) -> Self {
        Self {
            state: Mutex::new(State {
                api: 0,
                log: None,
                service: None,
                ready: None,
                native: None,
                owners: [None, None],
                reported: [false, false],
                logged: false,
                observed_reason: None,
                identity: None,
                early_loader: None,
                #[cfg(any(windows, test))]
                early_scanned: false,
            }),
            reason,
            worker: None,
            plan: plan_source::PlanCell::new(),
            own_loader: own_loader_in_process,
            status: std::sync::OnceLock::new(),
            native_run: OnceLock::new(),
            native: NativeHooks::PRODUCTION,
            startup: startup_seam::kiln_eml_export,
            seam: OnceLock::new(),
            outcome: OnceLock::new(),
            early_unwritable: OnceLock::new(),
            notice: OnceLock::new(),
            notice_hooks: notice_native::NoticeHooks::PRODUCTION,
        }
    }
    /// Replaces the native banner's ready-event probe and durations. Tests use it for a Kiln.Ui that is there.
    pub const fn with_notice(mut self, hooks: notice_native::NoticeHooks) -> Self {
        self.notice_hooks = hooks;
        self
    }
    /// This start's native do-not-save banner: `None` before it was started, and for every selection but `kiln`.
    pub fn notice(&self) -> Option<&Arc<notice_native::Notice>> {
        self.notice.get()
    }
    /// Replaces where kiln-eml's startup seam is found ( production: the loaded kiln-eml.dll's early export).
    /// Tests use it for a fake seam built from kiln-abi's types.
    pub const fn with_startup(mut self, resolve: startup_seam::Resolve) -> Self {
        self.startup = resolve;
        self
    }
    /// Whether the early phase attached to kiln-eml's startup seam this start.
    pub fn startup_attached(&self) -> bool {
        matches!(self.seam.get(), Some(SeamState::Attached(_)))
    }
    /// kiln-eml's startup outcome as Kiln's plugin step read it (`None` before, and for every selection but `kiln`
    /// with a frozen plan).
    pub fn startup_outcome(&self) -> Option<&Arc<StartupRead>> {
        self.outcome.get()
    }
    /// Replaces the native manager's wiring (the witness port, the thread spawn and the loader).
    /// Tests use it for a wired witness, a counting spawn and a fake loader backend.
    pub const fn with_native(mut self, native: NativeHooks) -> Self {
        self.native = native;
        self
    }
    /// The native manager's run of this start: made once, right after the plan read, under
    /// `kiln` with a worker; `None` for every other selection and before the read.
    pub fn native_run(&self) -> Option<&Arc<NativeRun>> {
        self.native_run.get()
    }
    /// Replaces the look for ShroudForge's own loader in this process (on Windows
    /// a module exporting `ShroudforgeStop`, none elsewhere). Tests use it for a
    /// loader that is there at main, or arrives between main and on_ready, and to count the looks
    /// ([`Self::look_for_own_loader`]).
    pub const fn with_own_loader(mut self, probe: fn() -> Option<String>) -> Self {
        self.own_loader = probe;
        self
    }
    /// The look for ShroudForge's own loader under `kiln`. On Windows it enumerates the process's modules and asks
    /// each for an export, which waits while another thread holds the loader lock, as a package DLL's initializer does.
    /// The early phase looks, before its native run is armed (no package DLL of kiln-sfi's has loaded), and keeps the
    /// answer. Kiln's plugin step (main, and on_ready right before the start, for a loader that arrives in
    /// between) looks again only when kiln-sfi's own run cannot be inside a package DLL's load: no run, or one whose
    /// loader is done ([`NativeRun::may_be_loading`]); otherwise (a DLL the deadline left initializing) it keeps the early
    /// answer and asks the loader nothing. A loader the early phase found is not looked for again. Without an early
    /// look (the early phase never ran, so no run was armed) the plugin step looks as before.
    fn look_for_own_loader(&self, phase: Phase) -> Option<String> {
        let early = self
            .state
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .early_loader
            .clone();
        let found = match (phase, early) {
            (Phase::Early, None) => (self.own_loader)(),
            (_, Some(Some(found))) => return Some(found),
            (Phase::Normal, Some(None))
                if self
                    .native_run
                    .get()
                    .is_some_and(|run| run.may_be_loading()) =>
            {
                return None;
            }
            (_, _) => (self.own_loader)(),
        };
        if matches!(phase, Phase::Early) {
            self.state
                .lock()
                .unwrap_or_else(|p| p.into_inner())
                .early_loader = Some(found.clone());
        }
        found
    }
    /// The admitted interpreter: when kiln.launch froze backend `kiln`, the Normal
    /// phase binds `worker` to its owner and installs `on_ready`, which submits it
    /// under that owner. Every other selection never binds or submits it.
    pub const fn with_worker(reason: InactiveReason, worker: &'static worker::Worker) -> Self {
        let mut boundary = Self::inactive(reason);
        boundary.worker = Some(worker);
        boundary
    }
    pub fn worker_started(&self) -> bool {
        self.worker.is_some_and(worker::Worker::started)
    }
    /// The worker this boundary starts under `kiln`, if it has one.
    pub fn worker(&self) -> Option<&'static worker::Worker> {
        self.worker
    }
    /// This start's frozen launch plan as kiln-sfi read it (`None`: no read, which is every
    /// selection but `kiln` with a worker).
    pub fn launch_plan(&self) -> Option<&plan_source::PlanRead> {
        self.plan.get()
    }
    /// How often the plan file was read (at most once per boundary, so once per process for the
    /// exported one).
    pub fn plan_reads(&self) -> u32 {
        self.plan.reads()
    }
    /// kiln-sfi's sfi.json record and writer for this start.
    pub fn status(&self) -> &std::sync::Arc<state::StateHub> {
        self.status
            .get_or_init(|| std::sync::Arc::new(state::StateHub::new()))
    }

    /// The record of a start kiln-sfi will not run after all: `refused`, as the first
    /// record (from main or on_ready) or after `starting` (the early phase wrote it). A
    /// write that fails is logged; the record stays in memory.
    fn record_refused(&self, identity: Option<state::Identity>, code: &str, text: &str) {
        let reason = state::Reason::new(code, text);
        let written = match identity {
            Some(identity) if self.status().phase().is_none() => {
                match self
                    .status()
                    .begin(identity, state::Phase::Refused, Some(reason))
                {
                    state::Begun::NotWritten(e) => Err(e),
                    _ => Ok(()),
                }
            }
            _ => self.status().refuse(reason),
        };
        self.log_write(written);
    }

    /// In the early phase, before the native run is made, armed or attached, this start's
    /// sfi.json record (`starting`) is written. False when it is not on disk: nothing is armed or attached then, and
    /// on_ready refuses the start (`state-unwritable`), as it refuses one whose record it cannot write itself.
    ///
    /// # Safety
    /// `api` passed `Boundary::enter`'s prefix checks.
    unsafe fn begin_early(
        &self,
        api: *const KilnApi,
        record: &LaunchRecord,
        why: &str,
        log: LogFn,
        owner: KilnOwner,
    ) -> bool {
        // SAFETY: the caller's contract.
        let identity = unsafe { state::identity::read(api, record) };
        self.status()
            .set_dir(unsafe { state::identity::state_dir(api) });
        match self.status().begin(identity, state::Phase::Starting, None) {
            state::Begun::Written
            | state::Begun::Already(state::Phase::Starting | state::Phase::Running) => true,
            state::Begun::Already(_) => false,
            state::Begun::NotWritten(error) => {
                let line = format!(
                    "kiln-sfi: Kiln's interpreter has this start ({why}), but kiln/state/sfi.json cannot be written in kiln-sfi's early phase ({error}): no package DLL is armed, kiln-eml's startup seam is not attached and no ShroudForge mod runs"
                );
                unsafe { log(owner, KILN_LOG_ERROR, kstr(&line)) };
                let _ = self.early_unwritable.set(error);
                false
            }
        }
    }

    /// kiln-eml's startup seam, resolved and attached once (the early phase only, before the loader is armed),
    /// with kiln.launch's key and the native plan's DLL count. `None` without one: an older kiln-eml, an export that
    /// cannot be used, or an attach kiln-eml refused (kiln.log says which).
    fn attach_seam(
        &self,
        record: &LaunchRecord,
        plan_id: [u8; 16],
        planned_dlls: usize,
        log: LogFn,
        owner: KilnOwner,
    ) -> Option<Arc<Seam>> {
        let state = self.seam.get_or_init(|| {
            let Some(getter) = (self.startup)() else {
                return SeamState::NotBuilt;
            };
            let refused = |code: &'static str, text: String| {
                unsafe { log(owner, KILN_LOG_WARN, kstr(&format!("kiln-sfi: {text}"))) };
                SeamState::Refused(code, text)
            };
            let Some(launch_id) = record.launch_id else {
                return refused(
                    "startup-attach-refused",
                    "kiln.launch has no launch id, so kiln-sfi cannot attach to kiln-eml's startup seam".into(),
                );
            };
            // SAFETY: the resolver gives kiln-eml's export (or a stand-in with its contract).
            match unsafe { Seam::bind(getter, launch_id, record.plan_hash, plan_id) } {
                Err(why) => refused("startup-seam-unusable", why),
                Ok(seam) => match seam.attach(u32::try_from(planned_dlls).unwrap_or(u32::MAX)) {
                    Ok(_) => SeamState::Attached(Arc::new(seam)),
                    Err(result) => refused(
                        "startup-attach-refused",
                        format!(
                            "kiln-eml refused kiln-sfi's attach to its startup seam ({})",
                            startup_seam::result_word(result)
                        ),
                    ),
                },
            }
        });
        match state {
            SeamState::Attached(seam) => Some(Arc::clone(seam)),
            _ => None,
        }
    }

    /// kiln-eml's startup outcome for this start, read once: from the attached seam, or why there is none.
    /// `None` without a frozen plan.
    fn startup(&self) -> Option<Arc<StartupRead>> {
        let Some(plan_source::PlanRead::Frozen(frozen)) = self.plan.get() else {
            return None;
        };
        Some(Arc::clone(self.outcome.get_or_init(|| {
            Arc::new(match self.seam.get() {
                None => StartupRead::missing("startup-attach-missing", ATTACH_MISSING),
                Some(SeamState::NotBuilt) => StartupRead::not_built(),
                Some(SeamState::Refused(code, text)) => StartupRead::missing(code, text.clone()),
                Some(SeamState::Attached(seam)) => {
                    seam.read_outcome(|id| frozen.plan.package(id).is_some())
                }
            })
        })))
    }

    /// At Kiln's plugin step, for a start the interpreter may run (`owner`: kiln-sfi's plugin owner): kiln-eml's
    /// startup outcome, read once; its cross-checked identity is granted ([`plan_source::grant_archive`]) and the read
    /// goes to the worker's runtime-create ([`plan_source::follow_startup`]), which admits by its verdict and refuses
    /// the runtime without a grant. A start whose early phase could not write its record is refused by on_ready.
    fn plugin_step_startup(&self, owner: KilnOwner) {
        if self.early_unwritable.get().is_some() {
            return;
        }
        // a boundary without a worker never read a plan: on_ready refuses it
        let Some(read) = self.startup() else {
            return;
        };
        if let Some(identity) = &read.grant {
            plan_source::grant_archive(owner, *identity);
        }
        plan_source::follow_startup(owner, read);
    }

    /// kiln-sfi refuses a start after kiln-eml's startup outcome could be read: sfi.json gets the outcome's rows,
    /// and each package whose data kiln-eml applied and whose Lua now does not run a warning that its data stays applied
    /// with its runtime stopped. Before the refused record is written (it is final).
    fn record_after_outcome(&self, code: &str, text: &str) {
        if self.status().phase().is_none() || !self.startup_attached() {
            return;
        }
        if let (Some(plan_source::PlanRead::Frozen(frozen)), Some(read)) =
            (self.plan.get(), self.startup())
        {
            plan_source::record_after_outcome(self.status(), &frozen.plan, &read, code, text);
        }
    }
    fn log_write(&self, written: Result<(), state::WriteError>) {
        // no state folder (a host without one): nothing to say
        if let Err(state::WriteError::Io(e)) = written {
            self.status().log(
                KILN_LOG_WARN,
                &format!("kiln-sfi: cannot write kiln/state/sfi.json ({e})"),
            );
        }
    }

    /// on_unload: the native banner taken down (no frame subscription, no demand: native subscribers unsubscribe before
    /// they unload), then the final flush, `ended`, unless the record is final already. It never touches the worker or
    /// waits for it.
    fn unload(&self) {
        if let Some(notice) = self.notice.get() {
            notice.close();
        }
        let written = self.status().end();
        self.log_write(written);
    }

    /// Makes this start's native do-not-save banner and puts its step on kiln-sfi's IO thread, once (`kiln`: after
    /// on_ready, whatever it decided, or at main for a start refused there). Before Kiln's plugin step nothing is
    /// identified yet: no banner.
    fn start_notice(&self) {
        let (api, log, service, owner) = {
            let state = self.state.lock().unwrap_or_else(|p| p.into_inner());
            (state.api, state.log, state.service, state.owners[1])
        };
        let (Some(log), Some(service), Some(owner)) = (log, service, owner) else {
            return;
        };
        let mut made = false;
        let notice = self.notice.get_or_init(|| {
            made = true;
            // SAFETY: `api` is stored only after Boundary::enter's prefix checks; Host::read checks each member against
            // the declared size.
            let host = unsafe { worker::Host::read(api as *const KilnApi) };
            Arc::new(notice_native::Notice::new(
                Arc::clone(self.status()),
                notice_native::Env {
                    log,
                    service,
                    session: host.session,
                    owner,
                    pid: std::process::id(),
                },
                self.notice_hooks,
            ))
        });
        if made && !notice.start() {
            unsafe {
                log(
                    owner,
                    KILN_LOG_WARN,
                    kstr(
                        "kiln-sfi: no IO thread for the do-not-save banner; kiln.log and sfi.json keep the warnings",
                    ),
                )
            };
        }
    }

    /// Kiln's native.policy for ShroudForge package DLLs this start, as Kiln's
    /// plugin step sees it: kiln.launch's frozen policy and `KilnApi.build_info().verified`, set
    /// when the interpreter has this start (`None` for every other selection). The native manager's
    /// run was made earlier from the record's `build_verified`; when the two disagree the
    /// interpreter is refused (`verified-disagrees`). It is also the runtime `register_dll` table's
    /// input.
    pub fn native_permission(&self) -> Option<NativePermission> {
        self.state.lock().unwrap_or_else(|p| p.into_inner()).native
    }

    /// The plugin step's on_ready under `kiln` (installed only then). ShroudForge's
    /// own loader can arrive between main and on_ready, so it is
    /// looked for once more right before the start (unless kiln-sfi's native run may still be inside a package DLL's
    /// load: [`Self::look_for_own_loader`]); if it is there, kiln-sfi stands
    /// aside with main's words, in kiln.log and in both phases' rows.
    fn ready(&self) {
        let (log, service, owners, reason, identity) = {
            let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
            (
                state.log,
                state.service,
                state.owners,
                state.ready.clone(),
                state.identity.take(),
            )
        };
        let (Some(log), Some(service), Some(owner), Some(reason)) =
            (log, service, owners[1], reason)
        else {
            return;
        };
        if let Some(why) = self.look_for_own_loader(Phase::Normal) {
            let text = stands_aside(&reason, &why);
            unsafe { log(owner, KILN_LOG_INFO, kstr(&text)) };
            // Readers see the oldest of equal reports (kiln_caps.h): the early
            // owner's, so both rows change.
            for owner in owners.into_iter().flatten() {
                let _ = unsafe { report_unavailable(log, service, owner, &text) };
            }
            self.record_after_outcome("shroudforge-in-process", &text);
            self.record_refused(identity, "shroudforge-in-process", &text);
            return;
        }
        let Some(worker) = self.worker else {
            let text = starts_text(&reason, false);
            self.record_after_outcome("no-worker", &text);
            self.record_refused(identity, "no-worker", &text);
            return;
        };
        // The start's record is on disk before any package effect, or nothing starts (the early phase wrote it
        // under `kiln`; this is the start whose early phase never ran).
        let begun = match (self.early_unwritable.get(), identity) {
            // the early phase could not write it: nothing was armed or attached
            (Some(error), _) => state::Begun::NotWritten(error.clone()),
            (None, Some(identity)) => self.status().begin(identity, state::Phase::Starting, None),
            (None, None) => self.status().phase().map_or(
                state::Begun::NotWritten(state::WriteError::NoFolder),
                state::Begun::Already,
            ),
        };
        match begun {
            state::Begun::Written
            | state::Begun::Already(state::Phase::Starting | state::Phase::Running) => {}
            // refused or ended before: never started now
            state::Begun::Already(_) => return,
            state::Begun::NotWritten(error) => {
                let text = format!(
                    "Kiln's interpreter has this start ({reason}), but kiln/state/sfi.json cannot be written ({error}): no ShroudForge mod runs"
                );
                unsafe { log(owner, KILN_LOG_ERROR, kstr(&format!("kiln-sfi: {text}"))) };
                for owner in owners.into_iter().flatten() {
                    let _ = unsafe { report_unavailable(log, service, owner, &text) };
                }
                let _ = self
                    .status()
                    .refuse(state::Reason::new("state-unwritable", &text));
                return;
            }
        }
        self.status().start_writer();
        worker.report_status_to(std::sync::Arc::clone(self.status()));
        // The plan read of main or the early phase; the worker follows it and never reads the file.
        if let Some(plan_source::PlanRead::Frozen(frozen)) = self.plan.get() {
            for (level, line) in plan_source::on_ready(self.status(), &frozen.plan) {
                unsafe { log(owner, level, kstr(&line)) };
            }
            worker.follow_plan(std::sync::Arc::clone(frozen));
        }
        // The native manager's run of the early phase; its terminal feeds admission and sfi.json.
        if let Some(run) = self.native_run.get() {
            worker.follow_native(Arc::clone(run));
        }
        // A refusal is the worker's own loud line and status. Main's rows said
        // the worker starts here; once it did not, both rows say why.
        let (code, why) = match worker.start() {
            Ok(_) | Err(worker::StartError::AlreadyStarted) => return,
            Err(worker::StartError::NotBound) => {
                ("worker-not-bound", "its worker was never bound".to_owned())
            }
            Err(worker::StartError::NoTasksService) => {
                ("no-tasks", "kiln.tasks is not available".to_owned())
            }
            Err(worker::StartError::Refused(_, refusal)) => {
                // The worker's detail, when it has one: why an ARGUMENT was not retried.
                let detail = worker
                    .shared()
                    .and_then(|s| s.status().detail)
                    .map(|d| format!("; {d}"))
                    .unwrap_or_default();
                (
                    "tasks-refused",
                    format!(
                        "kiln.tasks refused its worker ({}{detail})",
                        worker::refusal_text(refusal)
                    ),
                )
            }
        };
        let text = format!(
            "Kiln's interpreter has this start ({reason}), but {why}: no ShroudForge mod runs"
        );
        for owner in owners.into_iter().flatten() {
            let _ = unsafe { report_unavailable(log, service, owner, &text) };
        }
        self.record_after_outcome(code, &text);
        let written = self.status().refuse(state::Reason::new(code, &text));
        self.log_write(written);
    }

    /// The native manager's one run of this boundary (of the process, for the exported one),
    /// made from the frozen plan and the record's policy and build_verified, then armed or concluded
    /// ([`arming::arm`]). Only a run made in the early phase can be armed (package DLLs load in the
    /// early window). Never waits for the loader thread; a second call does nothing.
    ///
    /// In the early phase kiln-eml's startup seam is attached first ([`Self::attach_seam`]); with it the run's
    /// witness is the seam's and the run's terminal goes back to kiln-eml once it is known (at once when the run is
    /// concluded without the loader, after the loader's body when it is armed, or by the run's deadline waiter at
    /// ShroudForge's 15 s deadline counted from the arming). Without it the witness port is the hooks'
    /// ([`arming::not_wired`] in production).
    fn ensure_native(
        &self,
        frozen: &plan_source::FrozenPlan,
        permission: NativePermission,
        early: bool,
        log: LogFn,
        owner: KilnOwner,
        record: &LaunchRecord,
    ) {
        if self.native_run.get().is_some() {
            return;
        }
        let (plan, unreadable) = match NativePlan::from_launch_plan(&frozen.plan, &frozen.game) {
            Ok(plan) => (plan, None),
            Err(error) => (
                NativePlan {
                    plan_id: frozen.plan.plan_id(),
                    packages: Vec::new(),
                },
                Some(format!(
                    "the launch plan's native part cannot be read ({error}): no ShroudForge package DLL loads this start"
                )),
            ),
        };
        let planned_dlls = plan.packages.iter().map(|p| p.dlls.len()).sum();
        let seam = if early {
            self.attach_seam(record, plan.plan_id, planned_dlls, log, owner)
        } else {
            None
        };
        let seam_witness = seam
            .as_ref()
            .map(|seam| Arc::new(SeamWitness::new(Arc::clone(seam))));
        let port = match &seam_witness {
            Some(witness) => WitnessPort::Wired(Arc::clone(witness) as Arc<dyn Witness>),
            None => (self.native.witness)(),
        };
        let run = NativeRun::new(plan, port.witness(), permission);
        if self.native_run.set(Arc::clone(&run)).is_err() {
            return;
        }
        let push = seam.zip(seam_witness).map(|(seam, witness)| {
            let footprint = startup_seam::NativeFootprint::of(&frozen.plan);
            terminal_push(
                seam,
                witness,
                run.plan_id(),
                permission,
                footprint,
                log,
                owner,
            )
        });
        // made at Kiln's plugin step (the early phase never ran): concluded, unless the policy or an empty plan says
        // so first
        let late = (!early && permission.refusal().is_none() && run.dll_count() > 0)
            .then(|| arming::EARLY_PHASE_MISSED.to_owned());
        let armed = match unreadable.or(late) {
            Some(reason) => {
                run.conclude_without_loader(&reason);
                if let Some(push) = push {
                    push(&run.wait(std::time::Duration::ZERO));
                }
                Armed::NotArmed(reason)
            }
            None => {
                let body = || (self.native.loader)(Arc::clone(&run), log, owner);
                match push {
                    Some(push) => arming::arm_and_push(
                        &run,
                        &port,
                        self.native.spawn,
                        self.native.waiter,
                        self.native.deadline,
                        body,
                        push,
                    ),
                    None => arming::arm(&run, &port, self.native.spawn, body),
                }
            }
        };
        if let Armed::NotArmed(reason) = armed
            && run.dll_count() > 0
        {
            let text = format!("kiln-sfi: the native loader is not armed this start: {reason}");
            unsafe { log(owner, KILN_LOG_INFO, kstr(&text)) };
        }
    }

    #[cfg(any(windows, test))]
    fn apply_inactive_audit(&self, result: Result<Vec<String>, String>) -> KilnResult {
        let reason = match result {
            Ok(names) if names.is_empty() => None,
            Ok(names) => Some(native_enforcement::existing_runtime_reason(&names)),
            Err(error) => Some(format!(
                "native module audit unavailable; interpreter inactive: {error}"
            )),
        };
        // Inactivity is healthy, not an early-owner revocation. The actual
        // Enforcer::early audit still fails closed before any admission/load.
        let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
        state.early_scanned = true;
        state.observed_reason = reason;
        KILN_OK
    }

    /// # Safety
    /// The core must supply readable aligned table/header pointers and writable
    /// info storage covering its declared size. Short *declared* prefixes are
    /// checked before any tail read. Arbitrary invalid addresses are not Rust
    /// panic errors and cannot be made safe by catch_unwind.
    pub unsafe fn enter(
        &self,
        phase: Phase,
        api: *const KilnApi,
        owner: KilnOwner,
        info: *mut KilnPluginInfo,
    ) -> KilnResult {
        guard_result(|| unsafe { self.enter_inner(phase, api, owner, info) })
    }

    unsafe fn enter_inner(
        &self,
        phase: Phase,
        api: *const KilnApi,
        owner: KilnOwner,
        info: *mut KilnPluginInfo,
    ) -> KilnResult {
        if api.is_null() || info.is_null() || owner == 0 || !api.is_aligned() || !info.is_aligned()
        {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let size = unsafe { api.cast::<u32>().read() } as usize;
        if size < 8 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        if unsafe { api.cast::<u32>().add(1).read() } != KILN_ABI_VERSION
            || size < core::mem::offset_of!(KilnApi, service) + core::mem::size_of::<usize>()
            || unsafe { info.cast::<u32>().read() } < core::mem::size_of::<KilnPluginInfo>() as u32
        {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // Do not form &KilnApi: an older core may provide only this prefix.
        let log = unsafe { core::ptr::addr_of!((*api).log).read() };
        let service = unsafe { core::ptr::addr_of!((*api).service).read() };
        let (Some(log), Some(service)) = (log, service) else {
            return KILN_ERR_INVALID_ARGUMENT;
        };
        let index = match phase {
            Phase::Early => 0,
            Phase::Normal => 1,
        };
        let (needs_report, log_once) = {
            let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
            if state.api != 0 && state.api != api as usize {
                return KILN_ERR_INVALID_ARGUMENT;
            }
            if let Some(previous) = state.owners[index]
                && previous != owner
            {
                return KILN_ERR_INVALID_ARGUMENT;
            }
            // Reserve identity even if the required capability service fails.
            // Successful reporting is separate so identical re-entry can repair
            // a failed attempt without admitting a conflicting owner/API.
            state.api = api as usize;
            state.log = Some(log);
            state.service = Some(service);
            state.owners[index] = Some(owner);
            let log_once = !state.logged;
            state.logged = true;
            (!state.reported[index], log_once)
        }; // No host mutex is held across a service/log call, including re-entry.
        // The frozen selection first: it, not the early plugins' file
        // order, decides whether a ShroudForge runtime here is ShroudForge's own.
        let record = unsafe { read_launch_record(service) };
        let launch = record.launch.clone();
        let kiln = matches!(launch, Launch::Kiln { .. });
        #[cfg(windows)]
        let should_scan = (matches!(phase, Phase::Early) || kiln)
            && !self
                .state
                .lock()
                .unwrap_or_else(|p| p.into_inner())
                .early_scanned;
        #[cfg(windows)]
        if should_scan {
            // Read-only process-module audit at early entry, before admission.
            // No notifier, worker or DLL load is started while inactive.
            let result = self.apply_inactive_audit(native_enforcement::early_scan());
            if result != KILN_OK {
                return result;
            }
        }
        let audit = self
            .state
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .observed_reason
            .clone();
        // The early phase's look, kept for the plugin step unless kiln-sfi's run is done loading
        let own_loader = if kiln {
            self.look_for_own_loader(phase)
        } else {
            None
        };
        let off = env_off();
        // `refusal`: sfi.json's code when the frozen plan cannot be used (kiln, with a worker)
        let (reason, ready, refusal) = match select(
            &launch,
            off,
            audit.as_deref(),
            own_loader.as_deref(),
            self.reason,
        ) {
            // A boundary that can start reads the frozen plan once, in the first phase that
            // gets here (the early phase when it runs); every later phase and the worker use
            // that read
            Selection::Interpreter(why) => match self
                .worker
                // SAFETY: `api` passed the prefix checks above.
                .map(|_| unsafe { self.plan.get_or_read(api, &record) })
            {
                Some(plan_source::PlanRead::Refused { code, text }) => (
                    format!(
                        "Kiln's interpreter has this start ({why}), but {text}: no ShroudForge mod runs this start"
                    ),
                    None,
                    Some(*code),
                ),
                // the interpreter starts at Kiln's plugin step only (under the
                // Normal-phase owner); the early phase installs no on_ready
                read => {
                    // Right after the read, in the same phase, the native manager's one run; in the
                    // early phase this start's sfi.json record first (nothing is armed or attached without it)
                    if let (
                        Some(plan_source::PlanRead::Frozen(frozen)),
                        Launch::Kiln { native_policy, .. },
                    ) = (read, &launch)
                    {
                        let early = matches!(phase, Phase::Early);
                        // SAFETY: `api` passed the prefix checks above.
                        let begun =
                            !early || unsafe { self.begin_early(api, &record, &why, log, owner) };
                        if begun && self.early_unwritable.get().is_none() {
                            self.ensure_native(
                                frozen,
                                native_permission(*native_policy, record.build_verified),
                                early,
                                log,
                                owner,
                                &record,
                            );
                        }
                    }
                    (
                        starts_text(&why, self.worker.is_some()),
                        matches!(phase, Phase::Normal).then_some(why),
                        None,
                    )
                }
            },
            Selection::Inactive(reason) => (reason, None, None),
        };
        // Kiln's native.policy at Kiln's plugin step (the core sets `verified` after the early plugins ran), and a
        // cross-check: the native manager's run was made from the record's build_verified; when
        // build_info says otherwise now, the interpreter is refused (fail closed).
        let plugin_native = match (&ready, &launch) {
            (Some(_), Launch::Kiln { native_policy, .. }) => {
                Some(native_permission(*native_policy, unsafe {
                    read_build_verified(api)
                }))
            }
            _ => None,
        };
        let (reason, ready, refusal) = match (ready, plugin_native, self.native_run.get()) {
            (Some(why), Some(late), Some(run)) if late != run.permission() => (
                format!(
                    "Kiln's interpreter has this start ({why}), but kiln.launch's record says this game build is {} and build_info says {} at Kiln's plugin step: no ShroudForge mod runs this start",
                    verified_word(run.permission().verified),
                    verified_word(late.verified)
                ),
                None,
                Some("verified-disagrees"),
            ),
            (ready, _, _) => (reason, ready, refusal),
        };
        // At Kiln's plugin step: kiln-eml's startup outcome, read once (never from a file) and cross-checked, gives
        // the worker its grant and its verdict before any Lua; without a grant its runtime-create is refused
        if ready.is_some() {
            self.plugin_step_startup(owner);
        }
        if let Some(why) = ready.clone() {
            // The selection decides whether the worker is bound at all.
            if let Some(worker) = self.worker
                // SAFETY: `api` passed the prefix checks above; Host::read checks
                // each member against the declared size.
                && !worker.bind(unsafe { worker::Host::read(api) }, owner)
            {
                return KILN_ERR_INVALID_ARGUMENT;
            }
            let native = plugin_native;
            let first = {
                let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
                state.ready = Some(why);
                let first = state.native.is_none();
                state.native = native;
                first
            };
            if let Some(refusal) = native.and_then(NativePermission::refusal)
                && first
            {
                unsafe { log(owner, KILN_LOG_WARN, kstr(&native_refusal_text(refusal))) };
            }
        }
        let start = ready.is_some();
        if matches!(phase, Phase::Normal) {
            // sfi.json: aside or refused now; for a start, the identity waits for on_ready
            // SAFETY: `api` passed the prefix checks above.
            let identity = unsafe { state::identity::read(api, &record) };
            self.status()
                .set_dir(unsafe { state::identity::state_dir(api) });
            self.status().set_log(log, owner);
            if start {
                self.state
                    .lock()
                    .unwrap_or_else(|p| p.into_inner())
                    .identity = Some(identity);
            } else if kiln {
                let code = match refusal {
                    _ if off => "env-off",
                    Some(code) => {
                        // the loud warning: a plan the core froze that Kiln's interpreter cannot follow
                        unsafe {
                            log(
                                owner,
                                KILN_LOG_ERROR,
                                kstr(&format!("kiln-sfi: WARNING: {reason}")),
                            )
                        };
                        code
                    }
                    None => "shroudforge-in-process",
                };
                self.record_after_outcome(code, &reason);
                self.record_refused(Some(identity), code, &reason);
                // its warnings (kiln-eml's data applied, the runtime refused) need the banner too; no on_unload is
                // installed for this start, and a plugin is never unloaded, so nothing outlives the banner's state
                self.start_notice();
            } else {
                let decided = !matches!(launch, Launch::Absent | Launch::Idle { .. });
                let code = match &launch {
                    _ if off && decided => "env-off".to_owned(),
                    Launch::Absent => "no-kiln-launch".to_owned(),
                    _ => state::identity::reason_word(record.reason_code),
                };
                let written = match self.status().begin(
                    identity,
                    state::Phase::Aside,
                    Some(state::Reason::new(&code, &reason)),
                ) {
                    state::Begun::NotWritten(e) => Err(e),
                    _ => Ok(()),
                };
                self.log_write(written);
            }
        }
        unsafe {
            info.write(KilnPluginInfo {
                size: core::mem::size_of::<KilnPluginInfo>() as u32,
                id: kstr(ID),
                version: kstr(env!("CARGO_PKG_VERSION")),
                on_ready: start
                    .then_some(interpreter_ready as unsafe extern "C" fn(*mut core::ffi::c_void)),
                on_unload: start
                    .then_some(interpreter_unload as unsafe extern "C" fn(*mut core::ffi::c_void)),
                user: if start {
                    (self as *const Boundary).cast_mut().cast()
                } else {
                    core::ptr::null_mut()
                },
            });
        }
        // KILN_OK is intentional. The core revokes every non-OK main (main's
        // native/core/src/api.cpp:496-507, early_plugins.cpp:135-145);
        // disabled/default backend selection is not a plugin-load failure.
        if log_once {
            unsafe {
                log(owner, KILN_LOG_INFO, kstr(&reason));
            }
        }
        if needs_report {
            let result = unsafe { report_unavailable(log, service, owner, &reason) };
            if result != KILN_OK {
                return result;
            }
            self.state
                .lock()
                .unwrap_or_else(|p| p.into_inner())
                .reported[index] = true;
        }
        KILN_OK
    }
}

/// Reports `kiln-sfi.interpreter` unavailable for policy, with `reason`, as
/// `owner`'s row (a later report replaces it, kiln_caps.h).
///
/// # Safety
/// `log` and `service` are the core's `KilnApi` members.
unsafe fn report_unavailable(
    log: LogFn,
    service: ServiceFn,
    owner: KilnOwner,
    reason: &str,
) -> KilnResult {
    let table = unsafe { service(kstr("kiln.caps"), KILN_CAPS_VERSION) }.cast::<KilnCapsApi>();
    if table.is_null() || !table.is_aligned() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let header = table.cast::<KilnServiceHeader>();
    let bytes = unsafe { core::ptr::addr_of!((*header).size).read() } as usize;
    if bytes < core::mem::size_of::<KilnServiceHeader>()
        || unsafe { core::ptr::addr_of!((*header).version).read() } < KILN_CAPS_VERSION
        || bytes < core::mem::offset_of!(KilnCapsApi, report) + core::mem::size_of::<usize>()
    {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let Some(report) = (unsafe { core::ptr::addr_of!((*table).report).read() }) else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    let result = unsafe {
        report(
            owner,
            kstr(CAPABILITY),
            KILN_CAP_UNAVAILABLE as u32,
            KILN_CAP_REASON_POLICY as u32,
            kstr(reason),
        )
    };
    if result != KILN_OK {
        unsafe {
            log(
                owner,
                KILN_LOG_WARN,
                kstr("kiln-sfi: capability report refused"),
            );
        }
        return KILN_ERR_INTERNAL;
    }
    KILN_OK
}

/// The plugin step's on_ready for backend `kiln`. `user` is the Boundary that
/// installed it (the exported one is a static); null is ignored.
unsafe extern "C" fn interpreter_ready(user: *mut core::ffi::c_void) {
    guard_callback(|| {
        if let Some(boundary) = unsafe { user.cast_const().cast::<Boundary>().as_ref() } {
            boundary.ready();
            // whatever on_ready decided, the record's warnings get the banner
            boundary.start_notice();
        }
    });
}

/// on_unload for backend `kiln`: the final flush of sfi.json (`ended`). `user` as
/// for on_ready; null is ignored. Never waits for the worker.
unsafe extern "C" fn interpreter_unload(user: *mut core::ffi::c_void) {
    guard_callback(|| {
        // SAFETY: as in `interpreter_ready`.
        if let Some(boundary) = unsafe { user.cast_const().cast::<Boundary>().as_ref() } {
            boundary.unload();
        }
    });
}

fn kstr(text: &str) -> KilnStr {
    KilnStr {
        ptr: text.as_ptr().cast(),
        len: text.len(),
    }
}

/// Keep panic translation at every Rust ABI boundary, including future host
/// callbacks nested inside a guarded mod-DLL call. SEH is not a panic catcher.
pub fn guard_result(f: impl FnOnce() -> KilnResult) -> KilnResult {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(KILN_ERR_INTERNAL)
}
pub fn guard_callback(f: impl FnOnce()) -> bool {
    catch_unwind(AssertUnwindSafe(f)).is_ok()
}

/// The production worker: its runtime follows the frozen launch plan.
static WORKER: worker::Worker =
    worker::Worker::new(lifecycle::factory::<plan_source::KilnPlanSource>);
/// The exported boundary, bound to the production worker. Kiln's interpreter still runs only
/// when the core froze backend `kiln`, which needs the held capability kiln-sfi.interpreter.
static BOUNDARY: Boundary = Boundary::with_worker(InactiveReason::NegotiationMissing, &WORKER);

/// The boundary `kiln_plugin_early` and `kiln_plugin_main` enter (tests and support).
pub fn exported() -> &'static Boundary {
    &BOUNDARY
}

/// # Safety
/// Same caller contract as [`Boundary::enter`]. No Lua or mod code runs here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kiln_plugin_early(
    api: *const KilnApi,
    owner: KilnOwner,
    info: *mut KilnPluginInfo,
) -> KilnResult {
    guard_result(|| unsafe { BOUNDARY.enter(Phase::Early, api, owner, info) })
}
/// # Safety
/// Same caller contract as [`Boundary::enter`]. Normal and early owners differ.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kiln_plugin_main(
    api: *const KilnApi,
    owner: KilnOwner,
    info: *mut KilnPluginInfo,
) -> KilnResult {
    guard_result(|| unsafe { BOUNDARY.enter(Phase::Normal, api, owner, info) })
}

#[cfg(test)]
mod audit_tests {
    use super::*;

    #[test]
    fn every_boundary_looks_for_the_loader_with_the_real_probe_unless_a_test_replaces_it() {
        let real = own_loader_in_process as fn() -> Option<String>;
        let worker: &'static worker::Worker =
            Box::leak(Box::new(worker::Worker::new(worker::empty_runtime)));
        for boundary in [
            Boundary::new(),
            Boundary::default(),
            Boundary::inactive(InactiveReason::Hosted),
            Boundary::with_worker(InactiveReason::NegotiationMissing, worker),
        ] {
            assert!(core::ptr::fn_addr_eq(boundary.own_loader, real));
        }
        assert!(core::ptr::fn_addr_eq(BOUNDARY.own_loader, real));
        let replaced = Boundary::new().with_own_loader(|| None);
        assert!(!core::ptr::fn_addr_eq(replaced.own_loader, real));
    }

    #[test]
    fn shroudforge_zero_stands_every_decided_backend_aside_with_one_reason() {
        let fallback = InactiveReason::NegotiationMissing;
        let r = || "r".to_string();
        for launch in [
            Launch::Kiln {
                code: 5,
                reason: r(),
                native_policy: NativePolicy::Never,
            },
            Launch::Shroudforge {
                installed: false,
                code: 1,
                reason: r(),
            },
            Launch::Shroudforge {
                installed: true,
                code: 2,
                reason: r(),
            },
            Launch::Refused {
                code: 0x101,
                reason: r(),
            },
        ] {
            assert_eq!(
                select(&launch, true, None, None, fallback),
                Selection::Inactive("KILN_SHROUDFORGE=0".into()),
                "{launch:?}"
            );
        }
        // the core did not decide: the rules from before the table, the variable changes nothing
        assert_eq!(
            select(&Launch::Absent, true, None, None, fallback),
            Selection::Inactive(fallback.text().into())
        );
        let idle = Launch::Idle {
            code: 0x204,
            reason: r(),
        };
        assert_eq!(
            select(&idle, true, None, None, fallback),
            select(&idle, false, None, None, fallback)
        );
    }

    #[test]
    fn select_starts_the_interpreter_only_for_kiln_with_no_shroudforge_of_its_own() {
        let kiln = Launch::Kiln {
            code: 5,
            reason: "r".into(),
            native_policy: NativePolicy::Always,
        };
        let fallback = InactiveReason::NegotiationMissing;
        assert!(matches!(
            select(&kiln, false, None, None, fallback),
            Selection::Interpreter(_)
        ));
        for (audit, loader) in [
            (
                Some("ShroudForge's own loader (shroudforge-runtime.dll) runs it"),
                None,
            ),
            (
                Some("native module audit unavailable; interpreter inactive: x"),
                None,
            ),
            (
                None,
                Some("ShroudForge's own loader (C:\\g\\dbghelp.dll) runs it"),
            ),
        ] {
            let Selection::Inactive(text) = select(&kiln, false, audit, loader, fallback) else {
                panic!("{audit:?} {loader:?}");
            };
            assert!(text.ends_with("kiln-sfi stands aside"), "{text}");
            assert!(text.contains(audit.or(loader).unwrap()), "{text}");
        }
        // hosted with ShroudForge's runtime in the process (kiln-shroudforge's own):
        // the record, not the file order, says why kiln-sfi stands aside
        let hosted = Launch::Shroudforge {
            installed: false,
            code: 1,
            reason: "r".into(),
        };
        assert_eq!(
            select(
                &hosted,
                false,
                Some("ShroudForge's own loader (shroudforge-runtime.dll) runs it"),
                None,
                fallback
            ),
            Selection::Inactive("backend hosted: kiln-shroudforge has this start (r)".into())
        );
        // no table: today's rule, the audit's finding or the fallback text
        assert_eq!(
            select(&Launch::Absent, false, None, Some("x"), fallback),
            Selection::Inactive(fallback.text().into())
        );
        assert_eq!(
            select(&Launch::Absent, false, Some("a"), None, fallback),
            Selection::Inactive("a".into())
        );
    }
    #[test]
    fn inactive_scan_failure_is_quiet_success_and_never_starts_worker() {
        let boundary = Boundary::new();
        assert_eq!(
            boundary.apply_inactive_audit(Err("module enumeration unavailable".into())),
            KILN_OK
        );
        assert!(!boundary.worker_started());
        let state = boundary.state.lock().unwrap();
        assert!(state.early_scanned);
        assert!(
            state
                .observed_reason
                .as_deref()
                .unwrap()
                .contains("audit unavailable")
        );
    }
}

// No DllMain: never join, dispatch, or destroy an interpreter under loader lock.
// No on_ready/on_unload installed while inactive; backend `kiln` installs the
// plugin step's on_ready (interpreter_ready), which starts the worker (a boundary
// without one refuses), and on_unload (interpreter_unload), which only writes
// sfi.json's end. The admitted interpreter's worker is a kiln.tasks task: the exit
// signals it and parks it, and on_unload never waits for it.
