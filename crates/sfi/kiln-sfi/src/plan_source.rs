// SPDX-License-Identifier: MIT
//! The production plan source: where Kiln's interpreter learns what it runs.
//!
//! **One read per process.** Under backend `kiln` the launch plan the core froze is read once, in
//! kiln-sfi's early phase ([`PlanCell`], [`read_frozen`]); the main phase reads it only if the early
//! phase never ran, and the worker never reads it: it gets the same [`FrozenPlan`] in memory.
//! 1. The profile's text comes from kiln.profile (read once by the core, immutable); profile.json
//!    is never read from disk, and neither is kiln/state/launch.json.
//!    `sfLaunchPlan.path` must be `launch-plans/<canonical plan id>.json`.
//! 2. The plan is opened below `<kiln>/launch-plans` as the core opens it: a plain folder, no link
//!    or other reparse point, the opened handle checked to be the checked file, at most 8 MiB.
//!    The bytes read must hash to the record's `plan_hash`.
//! 3. The same bytes are parsed by sfi-policy's strict reader against this executable's hash and
//!    side (`build_info`), and no two of its packages may be one folder under two names
//!    (`LaunchPlan::check_folders`: an 8.3 name, a link). It may only remove what the core accepted: a plan
//!    it refuses is a `refused` start (`plan-invalid`), never a hosted one.
//!
//! **Admission** ([`admission`], pure) follows the frozen plan and removes what cannot run whole
//! (a package that cannot run whole is skipped with its reason plus the loud warning):
//! - only candidates run; `skip`, `disabled` and `other-side` packages never register;
//! - a package that ships native code runs only as Kiln's native manager concluded
//!   ([`native_verdict`]): the boundary's early phase made the one run of the process, and
//!   without kiln-eml's startup witness or when the policy refuses it is concluded without
//!   the loader, so every such package is withdrawn with that reason; a worker that got no run
//!   withdraws them too ([`NativeVerdict::NotWired`]). A package that only registers DLLs from its
//!   Lua (`runtime-register-dll`) is withdrawn when the loader is not armed and runs under an armed
//!   run (its registrations meet the runtime's register_dll table, which knows only planned DLLs);
//! - a package with startup data runs only when kiln-eml's startup outcome
//!   ([`crate::startup_seam`]) says its data is in the live generation ([`StartupVerdict::Committed`]);
//!   otherwise it is withdrawn with the outcome's code, text and chain (`startup-not-committed` when the
//!   outcome does not name it). Its Lua never runs over data that is not there. Without an outcome
//!   ([`StartupVerdict::NotBuilt`]: an older kiln-eml; [`StartupVerdict::Missing`]: none could be read)
//!   it is held. The one exception there is an EML package without a Lua part in a plan of kiln-eml's
//!   dialect (`eml`): kiln-eml's own EML pass applies its data, so nothing of it is withdrawn, and a
//!   package that depends on it waits for an outcome that names it (`startup-unwitnessed`);
//! - every package that depends on a removed one is removed too, through Lua-only packages.
//!
//! The plan's own `skip` decisions (selected packages the launcher left out) get their reason in
//! sfi.json, and the ShroudForge and native-selection ones the loud warning (kiln-eml says it for
//! its own kind). The native manager's terminal gives each planned DLL its row in sfi.json.
//!
//! At runtime-create the worker ([`KilnPlanSource`]) hashes each remaining package folder again
//! (`sfi_policy::digest`) and discovers `<game>/mods` (sfi-package's discovery, no ShroudForge profile
//! catalog); a changed or missing package is withdrawn with its dependents. Nothing here reads
//! below `<game>/shroudforge`.
//!
//! **game.types**: the archive generation granted to this start ([`grant_archive`],
//! filled by Kiln's plugin step from kiln-eml's cross-checked startup outcome) gives the worker
//! `game.types`, `runtime.types` and `game.version` from that generation's registry
//! ([`crate::runtime::types::ArchiveRegistryPort`] on kiln.eml.archive,
//! [`crate::runtime::types::wire`]): the composition's `game_types` and the runtime family `types`,
//! whose kiln.types cross-check logs under kiln-sfi's owner. The view is held only through that
//! runtime-create: the registry is an owned copy once read, and the port (with the view) drops with
//! the family and the composition at the end of create. **No grant, no runtime**:
//! a runtime-create without one is refused (`startup-grant-missing`, [`NO_GRANT`]) with
//! the loud warning and a refused sfi.json record before any package code, as ShroudForge's runtime
//! does not start without its type registry. So is one whose native run loaded a package DLL and could
//! not confirm afterwards that kiln-eml's witnessed input was unchanged (`startup-input-changed`).
//!
//! **Typed values**: every VM (runtime-create's and each live reload's new one) gets its own
//! kiln.eml.values arena over its own view of the same granted generation ([`PlanSource::values`]); the
//! types wiring's `game.types.of` answers typed values through the provider
//! ([`crate::runtime::values::type_index_of`]); and ShroudForge's unverified-operation reason names the
//! archive registry's game version as its game build ([`PlanSource::game_build`]).
use crate::LaunchRecord;
use crate::lifecycle::{PlanSource, RuntimePlan, Watchdog, process_clock};
use crate::native_manager::executor::{NativeTerminal, Outcome, STARTUP_NATIVE_DEADLINE};
use crate::runtime::overrides::{OverrideTrackers, session_world_kind};
use crate::runtime::types::{self as registry_types, ArchiveRegistryPort};
use crate::runtime::{Families, Family, ServiceAdapter};
use crate::startup_seam::StartupRead;
use crate::state::reducer::StartupRow;
use crate::state::{
    self, Change, Data, Dll, DllState, Intent, Lua, PackageInit, Part, PreviouslyActive,
    SaveSensitive, Skip, StateHub, TurnedOff, safety,
};
use crate::worker::{WorkerContext, compose::ComposePlan};
use kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1;
use kiln_abi::services::kiln_profile::*;
use kiln_abi::*;
use sfi_core::compose::GameTypes;
use sfi_core::host::{DiskSource, PackageSource, Side, SourceVersion};
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    ParseOptions,
    discover::{PackageRecord, Registry, discover_source},
    profiles::ProfileCatalog,
};
use sfi_policy::digest;
use sfi_policy::launch_plan::{
    Decision, Dialect, Kind, LaunchPlan, PartAdmission, PlanPackage, Severity, ValidationContext,
};
use sfi_policy::native_selection::Withdrawn;
use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};
use std::io::Read;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

/// The folder below `<game>/kiln` that holds launch plans.
pub const PLAN_FOLDER: &str = "launch-plans";
/// The largest launch plan read (the core's limit, kiln_launch.h).
pub const PLAN_MAX: u64 = 8 * 1024 * 1024;

/// The reason of a package withdrawn when no native manager run reached the worker (a fail-safe: the boundary makes a
/// run for every start it hands to the worker).
pub const NATIVE_NOT_WIRED: &str = "Kiln's native manager made no run for this start: no ShroudForge package DLL loads this start, so a package that ships one does not run";
/// The reason of a package held while no startup pass commits its data: kiln-eml offers no startup seam.
pub const STARTUP_NOT_BUILT: &str = "Kiln's EML loader (kiln-eml) offers no startup pass this start (it is not loaded, or older than Kiln's interpreter): this package's startup data was not applied, so its Lua does not run either; it is skipped whole and the game is as without the mod";
/// The reason of a package that depends on `dependency`, whose startup data kiln-eml's own EML pass
/// applies this start while no outcome of that pass reaches kiln-sfi (when no startup outcome names it).
fn unwitnessed(dependency: &str) -> String {
    format!(
        "it depends on {dependency}, whose startup data kiln-eml's EML pass applies this start; no outcome of that pass reaches Kiln's interpreter yet, so this package's Lua does not run"
    )
}

/// The startup data of `package` is kiln-eml's own EML pass's this start: an EML package without a
/// Lua part in a plan of kiln-eml's dialect (kiln-eml runs the admitted packages of an `eml`
/// plan as EML mods). Kiln's interpreter has nothing of it to run or to hold.
fn eml_pass_owns(dialect: Dialect, package: &PlanPackage<'_>) -> bool {
    dialect == Dialect::Eml && package.kind == Kind::Eml && !package.lua.required
}

/// The reason of a package with startup data that kiln-eml's startup outcome does not name as applied.
pub const NOT_COMMITTED: &str = "its startup data was not applied this start, so its Lua does not run either; it is skipped whole";

/// The reason of a package whose folder no longer has the plan's content digest.
pub const CHANGED: &str = "its files changed since the launcher wrote this start's launch plan; activate the profile in the Kiln launcher again";

/// The launch plan the core froze for this start, as kiln-sfi read and checked it.
#[derive(Debug)]
pub struct FrozenPlan {
    /// `launch-plans/<plan id>.json`
    pub name: String,
    /// SHA-256 of the bytes read, equal to the record's `plan_hash`.
    pub sha256: [u8; 32],
    /// kiln.launch's launch id of this start.
    pub launch_id: u64,
    /// This process's side (`build_info`), which is the plan's `game.side`.
    pub side: Side,
    /// `KilnApi.path(KILN_PATH_GAME)`: package roots are `<game>/mods/<folder>`.
    pub game: PathBuf,
    /// `KilnApi.path(KILN_PATH_CONFIG)` (the profile's configDir, else `<game>/kiln/config`), where the runtime
    /// publishes notices; `None` when the core gives none.
    pub config: Option<PathBuf>,
    pub plan: LaunchPlan,
}

/// The one read of this process's frozen plan.
#[derive(Debug)]
pub enum PlanRead {
    Frozen(Arc<FrozenPlan>),
    /// Kiln's interpreter does not run this start: sfi.json's `refused` code and the reason.
    Refused {
        code: &'static str,
        text: String,
    },
}

/// Holds the one read of the frozen plan (the exported boundary is a static, so one per process)
/// and counts how often the plan file was actually read.
#[derive(Debug, Default)]
pub struct PlanCell {
    read: OnceLock<PlanRead>,
    reads: AtomicU32,
}
impl PlanCell {
    pub const fn new() -> Self {
        Self {
            read: OnceLock::new(),
            reads: AtomicU32::new(0),
        }
    }
    /// The frozen plan, read now if it was not read before.
    ///
    /// # Safety
    /// As [`read_frozen`].
    pub unsafe fn get_or_read(&self, api: *const KilnApi, record: &LaunchRecord) -> &PlanRead {
        self.read.get_or_init(|| {
            self.reads.fetch_add(1, Ordering::AcqRel);
            // SAFETY: the caller's contract.
            unsafe { read_frozen(api, record) }
        })
    }
    /// The read, if one happened.
    pub fn get(&self) -> Option<&PlanRead> {
        self.read.get()
    }
    /// How often the plan was read (at most once).
    pub fn reads(&self) -> u32 {
        self.reads.load(Ordering::Acquire)
    }
}

type ServiceFn = unsafe extern "C" fn(KilnStr, u32) -> *const core::ffi::c_void;

/// Reads and checks the plan the record froze (see the module docs). Never panics; every
/// failure is a [`PlanRead::Refused`] with its reason.
///
/// # Safety
/// `api` passed `Boundary::enter`'s prefix checks (readable for its declared size).
pub unsafe fn read_frozen(api: *const KilnApi, record: &LaunchRecord) -> PlanRead {
    match unsafe { read_checked(api, record) } {
        Ok(frozen) => PlanRead::Frozen(Arc::new(frozen)),
        Err((code, text)) => PlanRead::Refused { code, text },
    }
}

type PlanRefusal = (&'static str, String);

unsafe fn read_checked(
    api: *const KilnApi,
    record: &LaunchRecord,
) -> Result<FrozenPlan, PlanRefusal> {
    let unreadable = |text: String| ("plan-unreadable", text);
    let launch_id = record
        .launch_id
        .ok_or_else(|| unreadable("this core has no kiln.launch record".into()))?;
    let service = unsafe { member_service(api) }
        .ok_or_else(|| unreadable("this core has no service lookup".into()))?;
    let profile = unsafe { profile_text(service) }.map_err(unreadable)?;
    let (name, id) = reference(&profile).map_err(unreadable)?;
    let kiln = unsafe { state::identity::api_path(api, KILN_PATH_KILN) }
        .ok_or_else(|| unreadable("the core gives no kiln folder".into()))?;
    let game = unsafe { state::identity::api_path(api, KILN_PATH_GAME) }
        .ok_or_else(|| unreadable("the core gives no game folder".into()))?;
    let bytes = read_plan_file(&kiln, &id).map_err(|why| unreadable(format!("{name}: {why}")))?;
    let sha256 = digest::sha256(&bytes);
    if sha256 != record.plan_hash {
        return Err((
            "plan-changed",
            format!(
                "{name} changed since this start froze it (its SHA-256 is not the one kiln.launch froze); activate the profile in the Kiln launcher again"
            ),
        ));
    }
    let (exe_sha256, side) = unsafe { build_identity(api) }
        .ok_or_else(|| unreadable("the core does not say which game executable runs".into()))?;
    let context = ValidationContext {
        exe_sha256,
        side: match side {
            Side::Client => "client".into(),
            Side::Server => "server".into(),
        },
        upstream_180_final: false,
    };
    let invalid = |why: String| ("plan-invalid", format!("{name} {why}"));
    let plan = LaunchPlan::parse(&bytes, &context).map_err(|why| {
        invalid(format!(
            "is not a launch plan Kiln's interpreter reads ({why})"
        ))
    })?;
    // two packages in one folder under two names (an 8.3 name, a link): the strict reader's overlap, by identity
    plan.check_folders(&game).map_err(|why| {
        invalid(format!(
            "is not a launch plan Kiln's interpreter reads ({why})"
        ))
    })?;
    if hex(&plan.plan_id()) != id.replace('-', "") {
        return Err(invalid(
            "has a plan id that is not the one its file name carries".into(),
        ));
    }
    if plan.permission(true) != sfi_policy::launch_plan::Permission::EligiblePendingAdmission {
        return Err(invalid("does not select Kiln's interpreter".into()));
    }
    let config = unsafe { state::identity::api_path(api, KILN_PATH_CONFIG) };
    Ok(FrozenPlan {
        name,
        sha256,
        launch_id,
        side,
        game,
        config,
        plan,
    })
}

unsafe fn member_service(api: *const KilnApi) -> Option<ServiceFn> {
    if api.is_null() || !api.is_aligned() {
        return None;
    }
    let size = unsafe { api.cast::<u32>().read() } as usize;
    if size < core::mem::offset_of!(KilnApi, service) + core::mem::size_of::<usize>() {
        return None;
    }
    unsafe { core::ptr::addr_of!((*api).service).read() }
}

/// kiln.profile's text (modes active and once: the profile this process booted with).
unsafe fn profile_text(service: ServiceFn) -> Result<String, String> {
    let name = KILN_PROFILE_SERVICE
        .strip_suffix(b"\0")
        .unwrap_or(KILN_PROFILE_SERVICE);
    let table = unsafe {
        service(
            KilnStr {
                ptr: name.as_ptr().cast(),
                len: name.len(),
            },
            KILN_PROFILE_VERSION,
        )
    }
    .cast::<KilnProfileApi>();
    let none = || "this start has no profile kiln.profile can give".to_owned();
    if table.is_null() || !table.is_aligned() {
        return Err(none());
    }
    // SAFETY: every table starts with its header; members are read only when the size covers them.
    let header = unsafe { table.cast::<KilnServiceHeader>().read() };
    if header.version < KILN_PROFILE_VERSION
        || (header.size as usize)
            < core::mem::offset_of!(KilnProfileApi, json) + core::mem::size_of::<usize>()
    {
        return Err(none());
    }
    let (Some(mode), Some(json)) = (unsafe {
        (
            core::ptr::addr_of!((*table).mode).read(),
            core::ptr::addr_of!((*table).json).read(),
        )
    }) else {
        return Err(none());
    };
    // SAFETY: core members; the text stays valid for the life of the process (kiln_profile.h).
    let mode = unsafe { mode() } as i32;
    if mode != KILN_PROFILE_ACTIVE && mode != KILN_PROFILE_ONCE {
        return Err(none());
    }
    let text = unsafe { json() };
    if text.ptr.is_null() || text.len == 0 {
        return Err(none());
    }
    let bytes = unsafe { std::slice::from_raw_parts(text.ptr.cast::<u8>(), text.len) };
    String::from_utf8(bytes.to_vec()).map_err(|_| "the profile is not UTF-8".to_owned())
}

/// The profile's `sfLaunchPlan` as the core reads it: exactly `{path, sha256}`, the path
/// `launch-plans/<canonical plan id>.json`. Returns the path and the plan id.
fn reference(profile: &str) -> Result<(String, String), String> {
    let doc: serde_json::Value = serde_json::from_str(profile.trim_start_matches('\u{feff}'))
        .map_err(|_| "the profile does not parse".to_owned())?;
    let reference = doc
        .get("sfLaunchPlan")
        .ok_or("the profile names no launch plan (sfLaunchPlan)")?;
    let shape = || {
        "the profile's sfLaunchPlan is not {\"path\": \"launch-plans/<plan id>.json\", \"sha256\": \"<64 hex digits>\"}"
            .to_owned()
    };
    let object = reference.as_object().ok_or_else(shape)?;
    let (Some(path), Some(sha256)) = (
        object.get("path").and_then(|p| p.as_str()),
        object.get("sha256").and_then(|s| s.as_str()),
    ) else {
        return Err(shape());
    };
    if object.len() != 2 || !lower_hex(sha256, 64) {
        return Err(shape());
    }
    let id = path
        .strip_prefix(PLAN_FOLDER)
        .and_then(|rest| rest.strip_prefix('/'))
        .and_then(|rest| rest.strip_suffix(".json"))
        .filter(|id| canonical_uuid(id))
        .ok_or_else(|| {
            format!("the profile's sfLaunchPlan path {path:?} is not launch-plans/<plan id>.json")
        })?;
    Ok((path.to_owned(), id.to_owned()))
}

/// `<kiln>/launch-plans/<id>.json`, read through one checked handle (see the module docs).
fn read_plan_file(kiln: &std::path::Path, id: &str) -> Result<Vec<u8>, String> {
    let folder = kiln.join(PLAN_FOLDER);
    if !digest::plain_folder(&folder) {
        return Err(format!("kiln/{PLAN_FOLDER} is not a plain folder"));
    }
    let file = digest::open_plain_file(&folder.join(format!("{id}.json")))?;
    let mut bytes = Vec::new();
    file.take(PLAN_MAX + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > PLAN_MAX {
        return Err("it is larger than 8 MiB".into());
    }
    Ok(bytes)
}

/// `build_info`'s executable hash (64 lower-case hex digits) and side; served in the early phase too.
unsafe fn build_identity(api: *const KilnApi) -> Option<(String, Side)> {
    let size = unsafe { api.cast::<u32>().read() } as usize;
    if size < core::mem::offset_of!(KilnApi, build_info) + core::mem::size_of::<usize>() {
        return None;
    }
    let build_info = unsafe { core::ptr::addr_of!((*api).build_info).read() }?;
    // SAFETY: an all-zero record is valid; the core fills it during the call.
    let mut info: KilnBuildInfo = unsafe { core::mem::zeroed() };
    info.size = core::mem::size_of::<KilnBuildInfo>() as u32;
    if unsafe { build_info(&mut info) } != KILN_OK {
        return None;
    }
    let exe: String = info
        .exe_sha256
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8 as char)
        .collect();
    if !lower_hex(&exe, 64) {
        return None;
    }
    let side = if info.side == KILN_SIDE_SERVER as u32 {
        Side::Server
    } else {
        Side::Client
    };
    Some((exe, side))
}

/// A lower-case SHA-256 in hex as bytes (`None` for anything else).
fn sha256_bytes(text: &str) -> Option<[u8; 32]> {
    if !lower_hex(text, 64) {
        return None;
    }
    let mut out = [0u8; 32];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).ok()?;
    }
    Some(out)
}

fn lower_hex(text: &str, len: usize) -> bool {
    text.len() == len
        && text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn canonical_uuid(text: &str) -> bool {
    text.len() == 36
        && text.bytes().enumerate().all(|(i, b)| {
            if matches!(i, 8 | 13 | 18 | 23) {
                b == b'-'
            } else {
                b.is_ascii_digit() || (b'a'..=b'f').contains(&b)
            }
        })
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

// ---- admission ----

/// What Kiln's native manager concluded for this start's package DLLs.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum NativeVerdict {
    /// No native manager run reached the worker: every package that ships native code is withdrawn.
    #[default]
    NotWired,
    /// The manager's terminals ([`native_verdict`]): the packages it refused, with its reasons.
    /// Every other package's DLLs loaded.
    Concluded { refused: BTreeMap<String, String> },
}

/// The native manager's terminal as admission's verdict.
/// - A run concluded without the loader (the policy refused, the plan lists no DLL, or no startup
///   witness is wired) refuses every candidate that ships native code with the run's reason: a
///   planned DLL, or `runtime-register-dll` (with no loader, nothing can register either).
/// - An executed run refuses the packages whose own DLL did not load. Their dependents follow in
///   admission, through Lua-only packages, along the same included dependencies the run withdrew
///   them by.
pub fn native_verdict(plan: &LaunchPlan, terminal: &NativeTerminal) -> NativeVerdict {
    let mut refused = BTreeMap::new();
    if let Some(reason) = &terminal.not_armed {
        for package in plan
            .packages()
            .filter(|p| p.decision == Decision::Candidate && p.has_native())
        {
            let why = if package.dlls.is_empty() {
                registers_without_loader(reason)
            } else {
                reason.clone()
            };
            refused.insert(package.id.to_owned(), why);
        }
    } else {
        for withdrawn in terminal.withdrawn.iter().filter(|w| w.caused_by.is_empty()) {
            refused.insert(withdrawn.mod_id.clone(), withdrawn.reason.clone());
        }
    }
    NativeVerdict::Concluded { refused }
}

/// Why a candidate that registers DLLs from its Lua (`runtime-register-dll`) does not run when the native loader was
/// never armed (`reason`): with no loader, nothing can register either. The native terminal says the same to kiln-eml
/// ([`crate::startup_seam::NativeFootprint`]).
pub fn registers_without_loader(reason: &str) -> String {
    format!(
        "it registers DLLs from its Lua, and Kiln's native loader is not armed this start ({reason})"
    )
}

/// Why kiln-eml's startup outcome left a package's data out: its code, text and cause chain (nearest first).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Refusal {
    pub code: String,
    pub text: String,
    pub chain: Vec<String>,
}

/// What kiln-eml's startup pass said about this start's startup data ([`crate::startup_seam`]).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum StartupVerdict {
    /// No startup seam (an older kiln-eml): every candidate with startup data is held.
    #[default]
    NotBuilt,
    /// No outcome could be read (`code`, why): every candidate with startup data is held with that reason.
    Missing { code: String, text: String },
    /// The outcome's rows: `packages` are those whose startup data is in the live generation (committed or vouched for
    /// by the cache); `refused` those it left out, with why. Any other candidate with startup data is held.
    Committed {
        packages: BTreeSet<String>,
        refused: BTreeMap<String, Refusal>,
    },
}

/// The facts admission follows besides the plan. The defaults are the safe ones: no native
/// manager run, no startup pass (until kiln-eml's startup outcome says otherwise).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Verdicts {
    pub native: NativeVerdict,
    pub startup: StartupVerdict,
}

/// A candidate that does not run whole this start.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Withdrawal {
    pub id: String,
    /// sfi.json's skip code.
    pub code: String,
    /// The part that could not run (sfi.json's skip part).
    pub part: Part,
    pub reason: String,
    /// The dependency chain to the package that could not run, nearest first (empty for that one).
    pub chain: Vec<String>,
}
impl Withdrawal {
    /// The same, as the composition's whole-package withdrawal.
    pub fn withdrawn(&self) -> Withdrawn {
        Withdrawn {
            mod_id: self.id.clone(),
            reason: self.reason.clone(),
            caused_by: self.chain.clone(),
        }
    }
}

/// What the runtime registers and runs this start.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Admission {
    /// Dependency first, in the plan's load order: the packages with an admitted Lua part and the
    /// candidates they depend on.
    pub order: Vec<String>,
    /// The packages whose Lua part runs.
    pub admitted: BTreeSet<String>,
    /// Every candidate that does not run whole, in the plan's load order.
    pub withdrawn: Vec<Withdrawal>,
}
impl Admission {
    pub fn is_withdrawn(&self, id: &str) -> bool {
        self.withdrawn.iter().any(|w| w.id == id)
    }
    /// Withdraws candidate `id` (a check at runtime-create) with every package that depends on it.
    /// A package that is not a candidate, or was withdrawn already, changes nothing.
    pub fn withdraw(
        &mut self,
        plan: &LaunchPlan,
        id: &str,
        code: &str,
        part: Part,
        reason: impl Into<String>,
    ) {
        if self.is_withdrawn(id) || !plan.load_order().contains(&id) {
            return;
        }
        self.withdrawn.push(Withdrawal {
            id: id.to_owned(),
            code: code.to_owned(),
            part,
            reason: reason.into(),
            chain: Vec::new(),
        });
        self.settle(plan);
    }
    /// The composition's withdrawals.
    pub fn compose_withdrawn(&self) -> Vec<Withdrawn> {
        self.withdrawn.iter().map(Withdrawal::withdrawn).collect()
    }

    /// Withdraws the dependents of every withdrawn package (included dependencies, whatever the
    /// dependent ships: through Lua-only packages too), then recomputes the order and the
    /// admitted set.
    fn settle(&mut self, plan: &LaunchPlan) {
        let order = plan.load_order();
        // a dependency always comes before its dependent in the load order
        for &id in &order {
            if self.is_withdrawn(id) {
                continue;
            }
            let Some(package) = plan.package(id) else {
                continue;
            };
            let cause = package
                .dependencies
                .iter()
                .filter(|d| d.included)
                .find_map(|d| self.withdrawn.iter().find(|w| w.id == d.id));
            if let Some(cause) = cause {
                let mut chain = vec![cause.id.clone()];
                chain.extend(cause.chain.iter().cloned());
                let reason = format!("depends on withdrawn package {}", cause.id);
                self.withdrawn.push(Withdrawal {
                    id: id.to_owned(),
                    code: "dependency-skipped".to_owned(),
                    part: Part::Package,
                    reason,
                    chain,
                });
            }
        }
        let rank: BTreeMap<&str, usize> =
            order.iter().enumerate().map(|(i, id)| (*id, i)).collect();
        self.withdrawn
            .sort_by_key(|w| rank.get(w.id.as_str()).copied().unwrap_or(usize::MAX));
        let live = |id: &str| !self.withdrawn.iter().any(|w| w.id == id);
        self.admitted = order
            .iter()
            .copied()
            .filter(|id| live(id))
            .filter(|id| {
                plan.package(id)
                    .is_some_and(|p| p.lua.admission == PartAdmission::Admit)
            })
            .map(str::to_owned)
            .collect();
        // the runtime registers the admitted packages and what they depend on (as ShroudForge's
        // runtime candidates: the runtime packages and their dependencies)
        let mut needed: BTreeSet<String> = self.admitted.clone();
        for id in order.iter().rev() {
            if !needed.contains(*id) {
                continue;
            }
            if let Some(package) = plan.package(id) {
                for d in package.dependencies.iter().filter(|d| d.included) {
                    if live(d.id) {
                        needed.insert(d.id.to_owned());
                    }
                }
            }
        }
        self.order = order
            .into_iter()
            .filter(|id| needed.contains(*id) && live(id))
            .map(str::to_owned)
            .collect();
    }
}

/// Why the native verdict removes `package`, if it does.
fn native_refusal(
    package: &PlanPackage<'_>,
    verdict: &NativeVerdict,
) -> Option<(&'static str, String)> {
    match verdict {
        NativeVerdict::NotWired if package.has_native() => {
            Some(("native-not-wired", NATIVE_NOT_WIRED.to_owned()))
        }
        NativeVerdict::NotWired => None,
        NativeVerdict::Concluded { refused } => refused
            .get(package.id)
            .map(|why| ("native-refused", why.clone())),
    }
}

/// Admission over the frozen plan (pure; see the module docs). It never adds what the plan
/// does not admit: every package it keeps is a candidate.
pub fn admission(plan: &LaunchPlan, verdicts: &Verdicts) -> Admission {
    let dialect = plan.startup_dialect();
    let mut admission = Admission::default();
    // candidates whose startup data kiln-eml's own pass applies, with no outcome that names them
    let mut unwitnessed_data: BTreeSet<&str> = BTreeSet::new();
    for id in plan.load_order() {
        let Some(package) = plan.package(id) else {
            continue;
        };
        if package.decision != Decision::Candidate {
            continue;
        }
        let remove = |code: &str, part, reason: String, chain: Vec<String>| Withdrawal {
            id: id.to_owned(),
            code: code.to_owned(),
            part,
            reason,
            chain,
        };
        if let Some((code, reason)) = native_refusal(&package, &verdicts.native) {
            admission
                .withdrawn
                .push(remove(code, Part::Dll, reason, Vec::new()));
            continue;
        }
        if !package.data.required {
            continue;
        }
        match &verdicts.startup {
            // the outcome names what is in the live generation: nothing else with startup data runs
            StartupVerdict::Committed { packages, refused } => {
                if packages.contains(id) {
                    continue;
                }
                let withdrawal = match refused.get(id) {
                    Some(r) => remove(
                        &r.code,
                        crate::state::reducer::startup_part(&r.code),
                        r.text.clone(),
                        r.chain.clone(),
                    ),
                    None => remove(
                        "startup-not-committed",
                        Part::Data,
                        NOT_COMMITTED.to_owned(),
                        Vec::new(),
                    ),
                };
                admission.withdrawn.push(withdrawal);
            }
            // no outcome: kiln-eml's own EML data is its pass's (its dependents wait below); the rest is held
            _ if eml_pass_owns(dialect, &package) => {
                unwitnessed_data.insert(id);
            }
            StartupVerdict::NotBuilt => admission.withdrawn.push(remove(
                "startup-not-built",
                Part::Data,
                STARTUP_NOT_BUILT.to_owned(),
                Vec::new(),
            )),
            StartupVerdict::Missing { code, text } => admission.withdrawn.push(remove(
                code,
                Part::Data,
                format!(
                    "{text}; so this package's startup data is not known to be applied and its Lua does not run"
                ),
                Vec::new(),
            )),
        }
    }
    // a candidate that needs one of those packages waits with it (fail closed); what depends on it
    // follows in settle
    for id in plan.load_order() {
        if admission.is_withdrawn(id) {
            continue;
        }
        let Some(package) = plan.package(id) else {
            continue;
        };
        if let Some(dependency) = package
            .dependencies
            .iter()
            .find(|d| d.included && unwitnessed_data.contains(d.id))
        {
            admission.withdrawn.push(Withdrawal {
                id: id.to_owned(),
                code: "startup-unwitnessed".to_owned(),
                part: Part::Package,
                reason: unwitnessed(dependency.id),
                chain: vec![dependency.id.to_owned()],
            });
        }
    }
    admission.settle(plan);
    admission
}

// ---- the type registry ----

/// Why a runtime-create without an archive generation granted to it is refused (`startup-grant-missing`).
pub const NO_GRANT: &str = "no archive generation was granted to Kiln's interpreter this start, so ShroudForge's type registry cannot be loaded: no ShroudForge mod runs this start";

/// The archive generation each owner's runtime reads its type registry from.
static ARCHIVE_GRANTS: Mutex<Vec<(KilnOwner, KilnEmlArchiveIdentityV1)>> = Mutex::new(Vec::new());

/// Grants `owner`'s runtime (kiln-sfi's plugin owner) the archive generation `identity` pins:
/// its next runtime-create reads `game.types` from that generation's registry. Kiln's plugin step
/// calls it once per start with the identity of kiln-eml's startup outcome, after
/// [`crate::startup_seam`] cross-checked it. A later grant replaces one.
///
/// A runtime-create without a grant is refused ([`NO_GRANT`]), the way ShroudForge refuses a start
/// whose registry failed to load. A grant is kept for the process until it is replaced, and every
/// runtime-create reads it again: one is replaced whenever the generation or reload epoch changes,
/// since a stale grant's `acquire` fails and a refused registry stops runtime-create before any
/// package code.
pub fn grant_archive(owner: KilnOwner, identity: KilnEmlArchiveIdentityV1) {
    let mut grants = ARCHIVE_GRANTS.lock().unwrap_or_else(|e| e.into_inner());
    grants.retain(|(o, _)| *o != owner);
    grants.push((owner, identity));
}

fn granted_archive(owner: KilnOwner) -> Option<KilnEmlArchiveIdentityV1> {
    let grants = ARCHIVE_GRANTS.lock().unwrap_or_else(|e| e.into_inner());
    grants.iter().find(|(o, _)| *o == owner).map(|(_, i)| *i)
}

/// What kiln-eml's startup outcome told each owner's start.
static STARTUPS: Mutex<Vec<(KilnOwner, Arc<StartupRead>)>> = Mutex::new(Vec::new());

/// The startup outcome Kiln's plugin step read for `owner`'s start ([`crate::startup_seam::Seam::read_outcome`]): its
/// next runtime-create admits by its verdict and gives sfi.json its rows. A later one replaces it.
pub fn follow_startup(owner: KilnOwner, read: Arc<StartupRead>) {
    let mut startups = STARTUPS.lock().unwrap_or_else(|e| e.into_inner());
    startups.retain(|(o, _)| *o != owner);
    startups.push((owner, read));
}

/// The startup outcome `owner`'s start follows, if Kiln's plugin step read one.
fn startup_of(owner: KilnOwner) -> Option<Arc<StartupRead>> {
    let startups = STARTUPS.lock().unwrap_or_else(|e| e.into_inner());
    startups
        .iter()
        .find(|(o, _)| *o == owner)
        .map(|(_, r)| Arc::clone(r))
}

/// A refused runtime-create, before any package code: the loud warning, and sfi.json's `refused` record with
/// the outcome's rows (in memory; the hub's writer thread writes it, the worker never waits for a file). Returns the
/// create error.
fn refuse_create(
    cx: &WorkerContext<'_>,
    plan: &LaunchPlan,
    startup: Option<&StartupRead>,
    code: &str,
    text: &str,
) -> String {
    cx.log(
        KILN_LOG_ERROR,
        &format!("kiln-sfi: WARNING: Kiln's interpreter does not run this start: {text}"),
    );
    if let Some(hub) = cx.shared().status_hub() {
        if let Some(startup) = startup {
            record_after_outcome(hub, plan, startup, code, text);
        }
        hub.update(|record| {
            record.set_phase(state::Phase::Refused, Some(state::Reason::new(code, text)))
        });
    }
    format!("{code}: {text}")
}

/// An armed run that loaded a package DLL and could not confirm afterwards that kiln-eml's witnessed input is unchanged
/// (`NativeTerminal::coherent`, which admission's native verdict does not read): why Kiln's interpreter does not run
/// (no startup data, no interpreter, a loud warning naming the DLL packages that
/// loaded). A run that loaded nothing changed nothing the witness covers.
pub fn incoherent(terminal: &NativeTerminal) -> Option<String> {
    let Err(why) = &terminal.coherent else {
        return None;
    };
    let loaded: BTreeSet<&str> = terminal
        .records
        .iter()
        .filter(|r| matches!(r.outcome, Outcome::Loaded { .. }))
        .map(|r| r.mod_id.as_str())
        .collect();
    if loaded.is_empty() {
        return None;
    }
    Some(format!(
        "the game files kiln-eml witnessed before the ShroudForge package DLLs ran could not be confirmed unchanged after them ({why}); DLLs of {} loaded, so Kiln's interpreter does not run this start and the game goes on without ShroudForge mods",
        loaded.into_iter().collect::<Vec<_>>().join(", ")
    ))
}

/// Kiln's interpreter will not run this start after kiln-eml's startup outcome was read. sfi.json gets the
/// outcome's rows, and every package whose data is in the live generation and whose Lua part now does not run a skip
/// (`code`, part lua): its warning says the data is applied and its runtime stopped
/// (presence `data-applied-runtime-stopped`). In memory only; the refused record that follows writes it.
pub fn record_after_outcome(
    hub: &StateHub,
    plan: &LaunchPlan,
    startup: &StartupRead,
    code: &str,
    text: &str,
) {
    if startup.rows.is_empty() {
        return;
    }
    hub.update(|record| {
        let mut change = record.startup_rows(&startup.rows);
        for row in startup.rows.iter().filter(|r| r.data.present()) {
            if plan.package(&row.id).is_some_and(|p| p.lua.required) {
                change = change.max(record.skip(&row.id, Skip::new(code, Part::Lua, text)));
            }
        }
        change
    });
}

thread_local! {
    /// The types family of the wiring `plan` made; `families` takes it (both run on the worker
    /// thread at one runtime-create, in that order).
    static TYPES_FAMILY: RefCell<Option<Family>> = const { RefCell::new(None) };
    /// The runtime.ecs registry family on the same wiring's handle; `families` takes it.
    static ECS_FAMILY: RefCell<Option<Family>> = const { RefCell::new(None) };
    /// The runtime.ecs attribute family on the same wiring's archive generation; `families` takes it.
    static ATTRIBUTES_FAMILY: RefCell<Option<Family>> = const { RefCell::new(None) };
    /// `game.assets`' loaded-resource family on the same wiring's handle; `families` takes it.
    static ASSETS_FAMILY: RefCell<Option<Family>> = const { RefCell::new(None) };
    /// The override trackers `plan` made (its cleanup) and their world kind; `families`
    /// takes them for the tracker family.
    static TRACKERS: RefCell<Option<Rc<OverrideTrackers>>> = const { RefCell::new(None) };
    /// The types wiring's handle of runtime-create's `plan`; `game_build` reads the archive registry's game
    /// version through it (taken there, so it lives no longer than that runtime-create).
    static TYPES_HANDLE: RefCell<Option<registry_types::TypesHandle>> = const { RefCell::new(None) };
    /// The generation the last plan (runtime-create's or a rebuild's) wired `game.types` to; `values` takes
    /// it, so a VM's typed values are made over the generation its own types read.
    static VALUES_GRANT: RefCell<Option<KilnEmlArchiveIdentityV1>> = const { RefCell::new(None) };
}

/// The composition's `game_types` on the granted archive registry (`identity`), its family, the ECS registry family
/// on its handle (asking `world_kind`'s catalog) and the attribute family over the same generation, kept for
/// [`PlanSource::families`], with the typed values' `game.types.of` recognizer. Runtime-create's (`boot`) handle is
/// kept for [`PlanSource::game_build`].
fn wire_types(
    cx: &WorkerContext<'_>,
    identity: KilnEmlArchiveIdentityV1,
    world_kind: u32,
    boot: bool,
) -> Rc<dyn GameTypes> {
    TYPES_FAMILY.with_borrow_mut(|slot| *slot = None);
    ECS_FAMILY.with_borrow_mut(|slot| *slot = None);
    ATTRIBUTES_FAMILY.with_borrow_mut(|slot| *slot = None);
    ASSETS_FAMILY.with_borrow_mut(|slot| *slot = None);
    TYPES_HANDLE.with_borrow_mut(|slot| *slot = None);
    let owner = cx.owner();
    let host = *cx.shared().host();
    let services = Rc::new(ServiceAdapter::new(
        host,
        Rc::new(Watchdog::new(process_clock())),
    ));
    let wiring = registry_types::wire(
        ArchiveRegistryPort::new(services, owner, identity),
        move |level, text| host.log(owner, level, text),
    );
    TYPES_FAMILY.with_borrow_mut(|slot| *slot = Some(wiring.family));
    // `game.types.of` answers the typed values (ShroudForge's `type_of` over its value classes)
    wiring
        .handle
        .add_recognizer(Rc::new(crate::runtime::values::type_index_of));
    if boot {
        TYPES_HANDLE.with_borrow_mut(|slot| *slot = Some(wiring.handle.clone()));
    }
    ASSETS_FAMILY.with_borrow_mut(|slot| {
        *slot = Some(crate::runtime::assets::family(wiring.handle.clone()));
    });
    ECS_FAMILY.with_borrow_mut(|slot| {
        *slot = Some(crate::runtime::ecs::family(wiring.handle, world_kind));
    });
    ATTRIBUTES_FAMILY.with_borrow_mut(|slot| {
        *slot = Some(crate::runtime::ecs::attributes::family(identity));
    });
    wiring.game_types
}

// ---- the worker side ----

/// The production [`PlanSource`]: the frozen plan's admission, checked again against the disk on
/// the worker at runtime-create.
pub struct KilnPlanSource;

impl KilnPlanSource {
    /// The runtime plan of runtime-create (`boot`) or of a live reload's rebuild.
    fn runtime_plan(cx: &WorkerContext<'_>, boot: bool) -> Result<RuntimePlan, String> {
        let frozen = cx
            .shared()
            .frozen_plan()
            .cloned()
            .ok_or("no frozen launch plan reached the worker")?;
        // kiln-eml's startup outcome as Kiln's plugin step read it; no archive generation granted, no runtime
        let startup = startup_of(cx.owner());
        // A refused runtime-create gives sfi.json its refused record and the loud warning. A reload's rebuild only
        // answers the request (the running runtime is restored); after a runtime-create that ran, neither refusal
        // below can happen there (the grant and the native terminal are the start's).
        let refuse = |code: &str, text: &str| {
            if boot {
                refuse_create(cx, &frozen.plan, startup.as_deref(), code, text)
            } else {
                format!("{code}: {text}")
            }
        };
        let Some(identity) = granted_archive(cx.owner()) else {
            let (code, text) = match startup.as_deref().and_then(|s| s.refusal.clone()) {
                Some((code, why)) => (
                    code,
                    format!(
                        "{why}, so Kiln's interpreter has no archive generation and ShroudForge's type registry cannot be loaded: no ShroudForge mod runs this start"
                    ),
                ),
                None => ("startup-grant-missing", NO_GRANT.to_owned()),
            };
            return Err(refuse(code, &text));
        };
        // The native manager's terminal (at once when it was concluded without the loader; an
        // armed run is waited for up to ShroudForge's startup deadline)
        let native = cx
            .shared()
            .native_run()
            .map(|run| run.wait(STARTUP_NATIVE_DEADLINE));
        if let Some(why) = native.as_ref().and_then(incoherent) {
            return Err(refuse("startup-input-changed", &why));
        }
        let verdicts = Verdicts {
            native: native
                .as_ref()
                .map_or_else(NativeVerdict::default, |t| native_verdict(&frozen.plan, t)),
            startup: startup
                .as_ref()
                .map_or_else(StartupVerdict::default, |s| s.verdict.clone()),
        };
        let mut admission = admission(&frozen.plan, &verdicts);
        check_disk(&frozen, &mut admission);
        let registry = discover(&frozen, &mut admission);
        // sfi.json's rows are runtime-create's; a reload's rebuild leaves them to the runtime that runs
        if boot && let Some(hub) = cx.shared().status_hub() {
            let rows = startup.as_ref().map_or(&[][..], |s| &s.rows[..]);
            record_packages(hub, &frozen.plan, &admission, native.as_ref(), rows);
        }
        VALUES_GRANT.with_borrow_mut(|slot| *slot = Some(identity));
        // The override trackers are both the lifecycle's cleanup and a family.
        let trackers = OverrideTrackers::new();
        TRACKERS.with_borrow_mut(|slot| {
            *slot = Some(trackers.clone());
        });
        Ok(RuntimePlan {
            side: frozen.side,
            version: SourceVersion::Sf179,
            registry,
            plan: admission.order.clone(),
            admitted: admission.admitted.clone(),
            cleanup: trackers,
            locale: Rc::new(DefaultLocale),
            clock: process_clock(),
            // The runtime phase's facts (ShroudForge 1.7.9 mod-runtime/ingame/start_ingame_runtime.rs:43-51): patch
            // is false there; export follows ShroudForge's loader config `exports.enabled` (default
            // false, :31-34), which lives below <game>/shroudforge, so kiln mode keeps the default
            // until Kiln has a setting of its own. The startup phase's values are kiln-eml's.
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: None,
            compose: ComposePlan {
                game_types: Some(wire_types(
                    cx,
                    identity,
                    session_world_kind(frozen.side),
                    boot,
                )),
                runtime: None,
                // this start's withdrawals, and the plan's own skips of ShroudForge and native-selection
                // packages (Kiln's interpreter is the one that would have run them): one loud warning each
                // (kiln-eml says it for its own kind)
                withdrawn: admission
                    .compose_withdrawn()
                    .into_iter()
                    .chain(
                        plan_skips(&frozen.plan)
                            .filter(|(p, _)| {
                                matches!(p.kind, Kind::Shroudforge | Kind::NativeSelection)
                            })
                            .map(|(p, reason)| Withdrawn {
                                mod_id: p.id.to_owned(),
                                reason,
                                caused_by: Vec::new(),
                            }),
                    )
                    .collect(),
            },
        })
    }
}

impl PlanSource for KilnPlanSource {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        Self::runtime_plan(cx, true)
    }

    /// The same admission over the frozen plan, the content digest of each remaining package folder checked
    /// again and `<game>/mods` discovered again, as at runtime-create; no sfi.json rows (the runtime that is
    /// replaced still runs while this is built). A changed package is withdrawn here, and the lifecycle host
    /// refuses the reload for it (ShroudForge's `updated runtime mod '<id>' failed to activate`).
    fn rebuild(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        Self::runtime_plan(cx, false)
    }

    /// ShroudForge's unverified-operation reason names its archive's game version
    /// (compatibility/lib.rs:30-41, `schema.game_version`): here the game version of the granted archive's
    /// registry, the text `game.version` reads. The registry is read now, at runtime-create (once; the
    /// types family and `game.types` use the same read). A registry that cannot be read keeps
    /// [`GAME_BUILD_UNKNOWN`](crate::runtime::availability::GAME_BUILD_UNKNOWN); that runtime-create is refused
    /// at its first package anyway ([`registry_types::REGISTRY_FAILED`]). A reload keeps runtime-create's.
    fn game_build(_: &WorkerContext<'_>) -> String {
        TYPES_HANDLE
            .with_borrow_mut(Option::take)
            .and_then(|handle| handle.registry().ok())
            .map_or_else(
                || crate::runtime::availability::GAME_BUILD_UNKNOWN.to_owned(),
                |registry| registry.game_version().to_owned(),
            )
    }

    /// The archive generation granted to this start, as the plan just made wired `game.types` to it
    /// (runtime-create's, or a rebuild's); each VM's typed-value arena is made over a view of it. Without a grant
    /// there is no runtime at all (the plan refuses first).
    fn values(_: &WorkerContext<'_>) -> Option<KilnEmlArchiveIdentityV1> {
        VALUES_GRANT.with_borrow_mut(Option::take)
    }

    /// A package with startup data or with native code (planned DLLs or
    /// `runtime-register-dll`) is reloaded only by restarting the game.
    fn restart_only(cx: &WorkerContext<'_>, id: &str) -> Option<String> {
        let package = cx.shared().frozen_plan()?.plan.package(id)?;
        if package.data.required {
            Some(crate::runtime::reload::RESTART_DATA.to_owned())
        } else if package.has_native() {
            Some(crate::runtime::reload::RESTART_NATIVE.to_owned())
        } else {
            None
        }
    }

    /// The production registrar's list ([`crate::runtime::registrar`]) over this plan's parts: the types family
    /// when [`Self::plan`] wired `game.types` (the cross-check and `game.version` run there) and the ECS registry family
    /// on it, the network family, the UI family, the override tracker on the plan side's
    /// world; the registrar adds the world context, the placement and patch refusals and ShroudForge's
    /// unavailable answers for every member left. Without an archive grant there is no runtime at all (the
    /// runtime-create is refused before any family runs).
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        crate::runtime::registrar::production(crate::runtime::registrar::Parts {
            types: TYPES_FAMILY.with_borrow_mut(Option::take),
            ecs: ECS_FAMILY.with_borrow_mut(Option::take),
            attributes: ATTRIBUTES_FAMILY.with_borrow_mut(Option::take),
            assets: ASSETS_FAMILY.with_borrow_mut(Option::take),
            network: crate::runtime::network::production_family(),
            // shroudforge.ui and shroudforge.notifications over this start's UI files
            ui: crate::runtime::ui::family(crate::runtime::ui::production_transport(cx)),
            trackers: TRACKERS
                .with_borrow_mut(Option::take)
                .map(|trackers| trackers.family()),
        })
    }
}

/// Each package the runtime would register must still have the plan's content digest.
fn check_disk(frozen: &FrozenPlan, admission: &mut Admission) {
    for id in admission.order.clone() {
        if admission.is_withdrawn(&id) {
            continue;
        }
        let Some(package) = frozen.plan.package(&id) else {
            continue;
        };
        let root = frozen.game.join("mods").join(package.folder());
        match digest::package_content_sha256(&root) {
            Ok(found) if found == package.content_sha256 => {}
            Ok(_) => {
                admission.withdraw(&frozen.plan, &id, "package-changed", Part::Package, CHANGED)
            }
            Err(why) => admission.withdraw(
                &frozen.plan,
                &id,
                "package-unreadable",
                Part::Package,
                format!(
                    "its folder {} cannot be read as the launch plan pinned it ({why})",
                    package.root
                ),
            ),
        }
    }
}

/// Whether the folder discovery found (`found`, a name from the listing of `mods`) is the planned folder, the one
/// whose content digest was checked: the same name exactly, or the same folder by identity (Windows opens the planned
/// name whatever its case, so a plan may spell it otherwise). Never names compared without case: that joins folders the
/// filesystem keeps apart (the Kelvin sign and `k`, `ΑΣ` and `Ασ` on Linux), and the package would run from a folder
/// whose digest nobody checked. An identity not known (none, or a volume serial of 0) never matches.
fn same_package_folder(mods: &std::path::Path, found: &str, planned: &str) -> bool {
    if found == planned {
        return true;
    }
    let known = |path: &std::path::Path| match sfi_policy::same_folder::identity(path) {
        Ok(Some((serial, index))) if serial != 0 => Some((serial, index)),
        _ => None,
    };
    match (known(&mods.join(found)), known(&mods.join(planned))) {
        (Some(found), Some(planned)) => found == planned,
        _ => false,
    }
}

/// ShroudForge's discovery over `<game>/mods` (no profile catalog: ShroudForge's lives below
/// `<game>/shroudforge`). Every package the runtime registers must be found in its planned folder
/// under its planned id. `enabled` is the profile's, as the plan froze it.
fn discover(frozen: &FrozenPlan, admission: &mut Admission) -> Registry {
    let mods = frozen.game.join("mods");
    let options = ParseOptions {
        version: SourceVersion::Sf179,
        ..ParseOptions::default()
    };
    let report = DiskSource::new(&mods).and_then(|root| {
        discover_source(
            &root,
            |entry| Ok(Rc::new(DiskSource::new(mods.join(&entry.path))?) as Rc<dyn PackageSource>),
            options,
            &ProfileCatalog::default(),
            &[],
        )
    });
    let report = report.unwrap_or_default();
    for id in admission.order.clone() {
        if admission.is_withdrawn(&id) {
            continue;
        }
        let Some(package) = frozen.plan.package(&id) else {
            continue;
        };
        let folder = package.folder();
        let found = report.registry.get(&id);
        if found.is_some_and(|r| same_package_folder(&mods, r.directory(), folder)) {
            continue;
        }
        let detail = report
            .errors
            .iter()
            .find(|e| same_package_folder(&mods, &e.directory, folder))
            .map(|e| format!(" ({})", e.detail))
            .unwrap_or_default();
        admission.withdraw(
            &frozen.plan,
            &id,
            "package-not-found",
            Part::Package,
            format!(
                "it is not found as ShroudForge package '{id}' in {}{detail}",
                package.root
            ),
        );
    }
    let mut registry = Registry::default();
    for record in report.registry.values() {
        let mut parsed = record.parsed().clone();
        parsed.manifest.enabled = frozen
            .plan
            .package(&parsed.manifest.id)
            .is_some_and(|p| p.enabled);
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

/// sfi.json's rows: every package the plan lists, where kiln-eml's startup outcome says its data is, a DLL row for each
/// terminal of the native manager, and a skip for each withdrawal. In memory only; the hub's writer thread writes it (the worker
/// never waits for a file).
fn record_packages(
    hub: &StateHub,
    plan: &LaunchPlan,
    admission: &Admission,
    native: Option<&NativeTerminal>,
    startup: &[StartupRow],
) {
    hub.update(|record| {
        let mut change = Change::None;
        for package in plan.packages() {
            change = change.max(record.add_package(row(&package, admission)));
        }
        // kiln-eml's outcome: where each package's startup data is
        change = change.max(record.startup_rows(startup));
        for dll in native.map(|t| t.records.as_slice()).unwrap_or_default() {
            let name = dll.path.rsplit('/').next().unwrap_or(&dll.path);
            let state = match &dll.outcome {
                Outcome::Loaded { .. } => DllState::Loaded,
                Outcome::ForeignOwned { .. } => DllState::ForeignOwner,
                Outcome::Refused(_) => DllState::Refused,
                Outcome::Faulted(_) | Outcome::TimedOut(_) => DllState::Failed,
                Outcome::Withdrawn(_) => DllState::NotLoaded,
            };
            let sha256 = dll.actual_sha256.as_deref().unwrap_or(&dll.planned_sha256);
            change = change.max(record.set_dll(
                &dll.mod_id,
                Dll {
                    name: name.to_owned(),
                    sha256: sha256_bytes(sha256),
                    state,
                },
            ));
        }
        for w in &admission.withdrawn {
            let mut skip = Skip::new(&w.code, w.part, w.reason.clone());
            skip.chain = w.chain.clone();
            change = change.max(record.skip(&w.id, skip));
        }
        // a selected package the plan skips: its reason, so the row says why it does not run
        for (package, reason) in plan_skips(plan) {
            change =
                change.max(record.skip(package.id, Skip::new("plan-skip", Part::Package, reason)));
        }
        change
    });
}

/// The plan's own `skip` decisions (selected packages the launcher left out), each with the plan's
/// reasons as one text.
fn plan_skips(plan: &LaunchPlan) -> impl Iterator<Item = (PlanPackage<'_>, String)> {
    plan.packages()
        .filter(|p| p.decision == Decision::Skip)
        .map(|p| {
            let messages: Vec<&str> = p.reasons.iter().map(|r| r.message).collect();
            let reason = format!("this start's launch plan skips it: {}", messages.join("; "));
            (p, reason)
        })
}

fn row(package: &PlanPackage<'_>, admission: &Admission) -> PackageInit {
    let candidate = package.decision == Decision::Candidate;
    let withdrawn = admission.is_withdrawn(package.id);
    let mut init = PackageInit::selected(package.id, Some(package.version), None);
    init.intent = match package.decision {
        Decision::Candidate | Decision::Skip => Intent::Selected,
        Decision::Disabled => Intent::Disabled,
        Decision::OtherSide => Intent::OtherSide,
    };
    init.data = match (package.data.required, candidate && !withdrawn) {
        (false, _) => Data::NotNeeded,
        (true, false) => Data::Excluded,
        // committed by a startup pass that reports its own outcome
        (true, true) => Data::Uncertain,
    };
    init.lua = if !package.lua.required {
        Lua::NotNeeded
    } else if admission.admitted.contains(package.id) {
        Lua::Admitted
    } else {
        Lua::Excluded
    };
    // The plan says whether a package is save-sensitive, not on which basis: say it
    // conservatively (still save-sensitive, so the next start's turned-off check sees it).
    init.save_sensitive = if package.save_sensitive {
        SaveSensitive::unknown_mixed()
    } else {
        SaveSensitive::none()
    };
    init
}

/// ShroudForge's default locale until Kiln's UI language reaches the plan.
struct DefaultLocale;
impl LocaleSource for DefaultLocale {
    fn locale(&self) -> Option<String> {
        Some("en".to_owned())
    }
}

// ---- on_ready ----

/// What a package of the previous start is in this start's plan.
fn now_in(plan: &LaunchPlan, id: &str) -> safety::Now {
    match plan.package(id).map(|p| p.decision) {
        Some(Decision::Candidate | Decision::Skip) => safety::Now::Selected,
        Some(Decision::Disabled) => safety::Now::Disabled,
        Some(Decision::OtherSide) => safety::Now::OtherSide,
        None => safety::Now::Absent,
    }
}

fn saved_basis(previous: &PreviouslyActive) -> SaveSensitive {
    match (&previous.policy_row, previous.basis.as_str()) {
        (Some((id, version)), "policy") => SaveSensitive::policy(id, version, None),
        _ => SaveSensitive::unknown_mixed(),
    }
}

/// At on_ready, after `starting` is on disk: the save-sensitive packages the previous start had
/// active that this plan has switched off or no longer lists ([`safety::turned_off`] against
/// [`StateHub::previously_active`]), and the plan's own warnings: each one is returned as a
/// kiln.log line (level, text), and a `save-warning` naming packages that are off this start gives
/// them the same turned-off row.
pub fn on_ready(hub: &StateHub, plan: &LaunchPlan) -> Vec<(i32, String)> {
    let previous = hub.previously_active();
    let off = safety::turned_off(previous, |p| p.id.as_str(), |id| now_in(plan, id));
    let mut covered: BTreeSet<String> = BTreeSet::new();
    let mut rows: Vec<TurnedOff> = Vec::new();
    let intent = |now| match now {
        safety::Now::Disabled => Intent::Disabled,
        _ => Intent::Absent,
    };
    for (p, now) in off {
        covered.insert(p.id.clone());
        rows.push(TurnedOff {
            id: p.id.clone(),
            version: p.version.clone(),
            name: p.name.clone(),
            now: intent(now),
            save_sensitive: saved_basis(p),
            archive_present: None,
        });
    }
    let mut lines = Vec::new();
    for warning in plan.warnings() {
        let level = match warning.severity {
            Severity::Info => KILN_LOG_INFO,
            _ => KILN_LOG_WARN,
        };
        lines.push((
            level,
            format!(
                "kiln-sfi: the launch plan warns ({}): {}",
                warning.code, warning.message
            ),
        ));
        if warning.severity != Severity::SaveWarning {
            continue;
        }
        for id in warning.caused_by {
            let now = now_in(plan, id);
            if !matches!(now, safety::Now::Disabled | safety::Now::Absent)
                || !covered.insert(id.to_owned())
            {
                continue;
            }
            rows.push(TurnedOff {
                id: id.to_owned(),
                version: plan.package(id).map(|p| p.version.to_owned()),
                name: None,
                now: intent(now),
                // the plan carries no basis: say it conservatively
                save_sensitive: SaveSensitive::unknown_mixed(),
                archive_present: None,
            });
        }
    }
    if !rows.is_empty() {
        hub.update(move |record| {
            rows.into_iter()
                .map(|row| record.turned_off(row))
                .max()
                .unwrap_or(Change::None)
        });
    }
    lines
}
