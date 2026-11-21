// SPDX-License-Identifier: MIT
//! kiln-sfi's side of kiln.eml.startup v1 (`native/core/abi/services/kiln_eml_startup.h`,
//! docs/kiln/services/eml-startup.md), the startup handshake with kiln-eml in a kiln-backend start.
//!
//! - **Resolve** ([`kiln_eml_export`]): kiln-eml's early export `kiln_eml_startup_service`, from the kiln-eml.dll
//!   module already loaded in this process (Kiln runs kiln-eml's early phase first). No module or no export: an older
//!   kiln-eml without the seam ([`StartupVerdict::NotBuilt`]).
//! - **Attach** ([`Seam::attach`]): once per process, from kiln-sfi's early phase, after sfi.json's `starting` is on
//!   disk and before the native loader is armed, with the key kiln.launch gave kiln-sfi (launch id, plan hash) and the
//!   number of DLLs in the native plan.
//! - **Witness** ([`SeamWitness`]): the native run's witness port. `ready` is one `witness_wait` of
//!   [`WITNESS_WAIT_MS`] (kiln-sfi never waits on kiln-eml longer than that; a reason too long for the first buffer is
//!   asked again with timeout 0, which the header guarantees answers the same); only READY for this start's plan id
//!   is a witness. `recheck` is `witness_recheck` once every planned DLL is terminal: unchanged, or why not. A
//!   recheck kiln-eml no longer answers (its wait for the terminal ended, so a DLL may have loaded after its own check
//!   before its pass) confirms nothing and is not coherent, like any other failure to ask; a run that loaded no DLL
//!   changed nothing either way ([`crate::plan_source::incoherent`]).
//! - **Terminal** ([`terminal_json`], [`Seam::push_terminal`]): the native terminal as native-terminal.schema.json
//!   v1, pushed once when it is known (at once for a run concluded without the loader, after the loader's body for an
//!   armed one, or by its deadline waiter at ShroudForge's 15 s deadline counted from the arming, so inside kiln-eml's
//!   own wait even when a DLL's initializer hangs; `crate::native_manager::arming::arm_and_push`). For a run never
//!   armed its `withdrawn` names every package admission refuses for native code ([`NativeFootprint`]), the ones
//!   that register DLLs from their Lua too, so kiln-eml applies no startup data for a package kiln-sfi will not run.
//! - **Outcome** ([`Seam::read_outcome`]): read once, at Kiln's plugin step, from the seam and never from a file. The
//!   record (startup-outcome.schema.json v1) is read strictly and cross-checked against the identity `outcome` fills
//!   and the launch kiln-sfi knows (phase, launch id, plan id, plan hash, archive digest) before any of it is used: a
//!   record of another launch or an identity that disagrees is `startup-grant-mismatch`, with no grant. Only an
//!   identity checked here is ever granted ([`crate::plan_source::grant_archive`]).
//!
//! Every member is any-thread and never calls back (kiln_eml_startup.h). kiln-sfi calls attach and outcome on the boot
//! thread, witness_wait on its loader thread, witness_recheck and native_terminal on the thread that concluded the run
//! (the loader thread, or the run's deadline waiter while the loader thread is still inside a DLL's initializer).
//! Nothing here enters Lua or reads a file.
use crate::launch_plan::{Decision, LaunchPlan};
use crate::native_manager::executor::{NativeTerminal, Outcome, Witness, WitnessToken};
use crate::native_manager::selection::Withdrawn;
use crate::plan_source::{Refusal, StartupVerdict};
use crate::state::Data;
use crate::state::reducer::{StartupRow, sf_id};
use kiln_abi::services::kiln_eml_startup::*;
use kiln_abi::*;
use serde_json::{Map, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::c_char;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};

/// kiln-eml's early export (`KilnEmlStartupServiceFn`).
pub type Getter = unsafe extern "C" fn(u32) -> *const KilnEmlStartupApiV1;
/// Where kiln-sfi finds kiln-eml's export: [`kiln_eml_export`] in production, a stand-in in tests.
pub type Resolve = fn() -> Option<Getter>;

/// The module kiln-eml is loaded as (Kiln's plugins folder, `build.sh`).
pub const KILN_EML_MODULE: &str = "kiln-eml.dll";
/// The longest kiln-sfi waits for kiln-eml's startup witness before the first package DLL (2 s).
pub const WITNESS_WAIT_MS: u32 = 2000;
/// The largest outcome record read.
pub const OUTCOME_MAX: usize = 64 << 20;
/// The first buffer for a reason text, and the largest one asked again.
const REASON_FIRST: usize = 4096;
const REASON_MAX: usize = 1 << 20;
/// The schemas' bounds for free text (characters) and cause chains.
const TEXT_MAX: usize = 4096;
const CHAIN_MAX: usize = 64;
const ROWS_MAX: usize = 4096;
const WARNINGS_MAX: usize = 64;
const KEY_SIZE: u32 = core::mem::size_of::<KilnEmlStartupKeyV1>() as u32;
const WITNESS_SIZE: u32 = core::mem::size_of::<KilnEmlStartupWitnessV1>() as u32;
const IDENTITY_SIZE: u32 = core::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;
const _: () = assert!(KEY_SIZE == KILN_EML_STARTUP_KEY_SIZE);
const _: () = assert!(WITNESS_SIZE == KILN_EML_STARTUP_WITNESS_SIZE);
// published_identity was appended later (kiln.boot's read, never kiln-sfi's): the five members kiln-sfi binds end at 48, so a
// kiln-eml whose table is 48 bytes still binds (Seam::bind checks each member's extent)
const _: () = assert!(core::mem::size_of::<KilnEmlStartupApiV1>() == 56);

/// kiln-eml's export, from the kiln-eml.dll module this process already loaded (`GetModuleHandleW`: no load, no
/// reference taken; Kiln never unloads a plugin before exit). `None`: no such module, or it has no seam.
#[cfg(windows)]
pub fn kiln_eml_export() -> Option<Getter> {
    use core::ffi::c_void;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetModuleHandleW(name: *const u16) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    }
    let name: Vec<u16> = KILN_EML_MODULE.encode_utf16().chain([0]).collect();
    // SAFETY: a NUL-terminated module name.
    let module = unsafe { GetModuleHandleW(name.as_ptr()) };
    if module.is_null() {
        return None;
    }
    // SAFETY: the generated export name ends in its NUL.
    let address = unsafe { GetProcAddress(module, KILN_EML_STARTUP_SERVICE_EXPORT.as_ptr()) };
    if address.is_null() {
        return None;
    }
    // SAFETY: kiln_eml_startup.h declares the export with this signature (KilnEmlStartupServiceFn).
    Some(unsafe { core::mem::transmute::<*mut c_void, Getter>(address) })
}

/// kiln-sfi ships for Windows only; another host (the build host's tests) has no kiln-eml.dll. There the export is the
/// one a stand-in registered with [`register_host_export`], if any.
#[cfg(not(windows))]
pub fn kiln_eml_export() -> Option<Getter> {
    HOST_EXPORT.get().copied()
}
#[cfg(not(windows))]
static HOST_EXPORT: OnceLock<Getter> = OnceLock::new();
/// Off Windows only: the export a stand-in kiln-eml of this process serves (once; false if one was registered).
#[cfg(not(windows))]
pub fn register_host_export(getter: Getter) -> bool {
    HOST_EXPORT.set(getter).is_ok()
}

type AttachFn = unsafe extern "C" fn(*const KilnEmlStartupKeyV1, u32, *mut u64) -> KilnResult;
type WaitFn = unsafe extern "C" fn(
    *const KilnEmlStartupKeyV1,
    u32,
    *mut KilnEmlStartupWitnessV1,
    *mut c_char,
    usize,
    *mut usize,
) -> KilnResult;
type RecheckFn = unsafe extern "C" fn(
    *const KilnEmlStartupKeyV1,
    *const KilnEmlStartupWitnessV1,
    *mut c_char,
    usize,
    *mut usize,
) -> KilnResult;
type TerminalFn = unsafe extern "C" fn(*const KilnEmlStartupKeyV1, KilnStr) -> KilnResult;
type OutcomeFn = unsafe extern "C" fn(
    *const KilnEmlStartupKeyV1,
    *mut KilnEmlArchiveIdentityV1,
    *mut c_char,
    usize,
    *mut usize,
) -> KilnResult;

/// kiln-eml's startup table, bound for this launch: its five members (copied once, after the extent and null checks
/// of `KILN_EML_STARTUP_HAS`) and the key kiln-sfi read from kiln.launch.
pub struct Seam {
    attach: AttachFn,
    wait: WaitFn,
    recheck: RecheckFn,
    terminal: TerminalFn,
    outcome: OutcomeFn,
    launch_id: u64,
    plan_sha256: [u8; 32],
    plan_id: [u8; 16],
    /// The session attach issued (nonzero), once.
    session: OnceLock<u64>,
    pushed: AtomicBool,
}

/// A result code as a word, for texts.
pub fn result_word(result: KilnResult) -> String {
    match result {
        KILN_OK => "OK".into(),
        KILN_ERR_INVALID_ARGUMENT => "INVALID_ARGUMENT".into(),
        KILN_ERR_UNAVAILABLE => "UNAVAILABLE".into(),
        KILN_ERR_BUFFER_TOO_SMALL => "BUFFER_TOO_SMALL".into(),
        KILN_ERR_INTERNAL => "INTERNAL".into(),
        other => format!("result {other}"),
    }
}

impl Seam {
    /// Binds the table `getter` serves for version 1, for the launch kiln.launch names (`launch_id`, `plan_sha256`)
    /// and its plan (`plan_id`). Every member must lie inside the table's declared size and be non-null.
    ///
    /// # Safety
    /// `getter` is kiln-eml's export or a stand-in with its contract: it returns NULL or a table readable for its
    /// declared size that lives for the process.
    pub unsafe fn bind(
        getter: Getter,
        launch_id: u64,
        plan_sha256: [u8; 32],
        plan_id: [u8; 16],
    ) -> Result<Self, String> {
        // SAFETY: the caller's contract; the getter is any-thread and side-effect free.
        let table = unsafe { getter(KILN_EML_STARTUP_VERSION) };
        if table.is_null() {
            return Err("kiln-eml's startup seam does not serve version 1".into());
        }
        if !table.is_aligned() {
            return Err("kiln-eml's startup seam table is misaligned".into());
        }
        // SAFETY: every table starts with its header; members are read only when the size covers them.
        let header = unsafe { table.cast::<KilnServiceHeader>().read() };
        if header.version < KILN_EML_STARTUP_VERSION {
            return Err(format!(
                "kiln-eml's startup seam is version {}, not 1",
                header.version
            ));
        }
        macro_rules! member {
            ($name:ident) => {{
                let end = core::mem::offset_of!(KilnEmlStartupApiV1, $name)
                    + core::mem::size_of::<usize>();
                if (header.size as usize) < end {
                    return Err(format!(
                        "kiln-eml's startup seam has no {} member",
                        stringify!($name)
                    ));
                }
                // SAFETY: the declared size covers the slot.
                match unsafe { core::ptr::addr_of!((*table).$name).read() } {
                    Some(member) => member,
                    None => {
                        return Err(format!(
                            "kiln-eml's startup seam has no {} member",
                            stringify!($name)
                        ));
                    }
                }
            }};
        }
        Ok(Self {
            attach: member!(attach),
            wait: member!(witness_wait),
            recheck: member!(witness_recheck),
            terminal: member!(native_terminal),
            outcome: member!(outcome),
            launch_id,
            plan_sha256,
            plan_id,
            session: OnceLock::new(),
            pushed: AtomicBool::new(false),
        })
    }

    fn key(&self, session: u64) -> KilnEmlStartupKeyV1 {
        KilnEmlStartupKeyV1 {
            size: KEY_SIZE,
            flags: 0,
            launch_id: self.launch_id,
            plan_sha256: self.plan_sha256,
            session,
        }
    }

    /// The session attach issued.
    pub fn session(&self) -> Option<u64> {
        self.session.get().copied()
    }

    /// `attach` with `planned_dlls` (the native plan's DLL count): once; the session it issued, or its result.
    pub fn attach(&self, planned_dlls: u32) -> Result<u64, KilnResult> {
        if let Some(session) = self.session() {
            return Ok(session);
        }
        let key = self.key(0);
        let mut session = 0u64;
        // SAFETY: a sized key and a live out-pointer; kiln-eml keeps neither.
        let result = unsafe { (self.attach)(&raw const key, planned_dlls, &raw mut session) };
        if result != KILN_OK {
            return Err(result);
        }
        if session == 0 {
            return Err(KILN_ERR_INTERNAL);
        }
        Ok(*self.session.get_or_init(|| session))
    }

    /// One call that may fill a counted text: once with [`REASON_FIRST`] bytes, and after BUFFER_TOO_SMALL once more
    /// with the complete size (`again` says it is the second call). Returns the result, the text (only with OK and
    /// `required` > 0) and `required`.
    fn with_text(
        mut call: impl FnMut(*mut c_char, usize, *mut usize, bool) -> KilnResult,
    ) -> (KilnResult, Option<String>, usize) {
        let mut buffer = vec![0u8; REASON_FIRST];
        let mut required = 0usize;
        let mut result = call(
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            &raw mut required,
            false,
        );
        if result == KILN_ERR_BUFFER_TOO_SMALL && required > 0 && required <= REASON_MAX {
            buffer = vec![0u8; required];
            required = 0;
            result = call(
                buffer.as_mut_ptr().cast(),
                buffer.len(),
                &raw mut required,
                true,
            );
        }
        let text = (result == KILN_OK && required > 0 && required <= buffer.len())
            .then(|| String::from_utf8_lossy(&buffer[..required]).into_owned());
        (result, text, required)
    }

    /// `native_terminal` with `json` (one call per seam: a second push is refused here and never reaches kiln-eml).
    pub fn push_terminal(&self, json: &str) -> KilnResult {
        let Some(session) = self.session() else {
            return KILN_ERR_UNAVAILABLE;
        };
        if self.pushed.swap(true, Ordering::AcqRel) {
            return KILN_ERR_UNAVAILABLE;
        }
        let key = self.key(session);
        // SAFETY: a sized key and counted UTF-8 that lives through the call.
        unsafe {
            (self.terminal)(
                &raw const key,
                KilnStr {
                    ptr: json.as_ptr().cast(),
                    len: json.len(),
                },
            )
        }
    }

    /// kiln-eml's startup outcome for this launch, read and cross-checked (module docs). `known` says whether the plan
    /// lists a package id. Called once, at Kiln's plugin step.
    pub fn read_outcome(&self, known: impl Fn(&str) -> bool) -> StartupRead {
        let Some(session) = self.session() else {
            return StartupRead::missing(
                "startup-outcome-missing",
                "kiln-sfi never attached to kiln-eml's startup seam, so kiln-eml's startup outcome cannot be read",
            );
        };
        let key = self.key(session);
        let mut identity = blank_identity();
        let mut required = 0usize;
        // SAFETY: a sized key and identity, no buffer (capacity 0), a live required.
        let mut result = unsafe {
            (self.outcome)(
                &raw const key,
                &raw mut identity,
                core::ptr::null_mut(),
                0,
                &raw mut required,
            )
        };
        let mut json = Vec::new();
        if result == KILN_ERR_BUFFER_TOO_SMALL {
            if required == 0 || required > OUTCOME_MAX {
                return StartupRead::missing(
                    "startup-outcome-invalid",
                    format!("kiln-eml's startup outcome claims {required} bytes"),
                );
            }
            json = vec![0u8; required];
            identity = blank_identity();
            let mut again = 0usize;
            // SAFETY: as above, with a buffer of `required` bytes.
            result = unsafe {
                (self.outcome)(
                    &raw const key,
                    &raw mut identity,
                    json.as_mut_ptr().cast(),
                    json.len(),
                    &raw mut again,
                )
            };
            if result == KILN_OK && again != required {
                return StartupRead::missing(
                    "startup-outcome-invalid",
                    format!("kiln-eml's startup outcome is {again} bytes after it said {required}"),
                );
            }
        }
        match result {
            KILN_OK if !json.is_empty() => {}
            KILN_OK => {
                return StartupRead::missing(
                    "startup-outcome-invalid",
                    "kiln-eml's startup outcome is empty",
                );
            }
            KILN_ERR_UNAVAILABLE => {
                return StartupRead::missing(
                    "startup-outcome-missing",
                    "kiln-eml's startup outcome is not available (its startup pass did not finish this start)",
                );
            }
            other => {
                return StartupRead::missing(
                    "startup-outcome-missing",
                    format!(
                        "kiln-eml's startup outcome could not be read ({})",
                        result_word(other)
                    ),
                );
            }
        }
        check_outcome(
            &json,
            &identity,
            self.launch_id,
            self.plan_id,
            self.plan_sha256,
            known,
        )
    }
}

/// The check [`Seam::read_outcome`] makes of the record and the identity kiln-eml returned: the strict read against
/// startup-outcome.schema.json v1, and the cross-check against this launch (`launch_id` and `plan_sha256` from
/// kiln.launch, `plan_id` from the plan; `known`: the plan lists that package id).
pub fn check_outcome(
    json: &[u8],
    identity: &KilnEmlArchiveIdentityV1,
    launch_id: u64,
    plan_id: [u8; 16],
    plan_sha256: [u8; 32],
    known: impl Fn(&str) -> bool,
) -> StartupRead {
    let launch = Launch {
        launch_id,
        plan_id,
        plan_sha256,
    };
    match parse_outcome(json, identity, &launch, &known) {
        Ok(read) => read,
        Err(Rejected::Mismatch(why)) => StartupRead::missing(
            "startup-grant-mismatch",
            format!("kiln-eml's startup outcome does not match this start ({why})"),
        ),
        Err(Rejected::Invalid(why)) => StartupRead::missing(
            "startup-outcome-invalid",
            format!("kiln-eml's startup outcome is not one Kiln's interpreter reads ({why})"),
        ),
    }
}

fn blank_identity() -> KilnEmlArchiveIdentityV1 {
    // SAFETY: integers and byte arrays: all-zero is valid.
    let mut identity: KilnEmlArchiveIdentityV1 = unsafe { core::mem::zeroed() };
    identity.size = IDENTITY_SIZE;
    identity
}

/// Why a recheck kiln-eml no longer answers is not coherent.
pub const RECHECK_GONE: &str = "kiln-eml no longer answers the recheck (its wait for the native terminal had ended, so a DLL may have loaded after its own check), so the input it witnessed cannot be confirmed unchanged after the DLLs";

/// The witness port on the seam.
pub struct SeamWitness {
    seam: std::sync::Arc<Seam>,
    /// [`WITNESS_UNASKED`], [`WITNESS_ISSUED`] or [`WITNESS_REFUSED`].
    state: AtomicU8,
}
const WITNESS_UNASKED: u8 = 0;
const WITNESS_ISSUED: u8 = 1;
const WITNESS_REFUSED: u8 = 2;
impl SeamWitness {
    pub fn new(seam: std::sync::Arc<Seam>) -> Self {
        Self {
            seam,
            state: AtomicU8::new(WITNESS_UNASKED),
        }
    }
    /// kiln-eml answered READY for this start's plan.
    pub fn issued(&self) -> bool {
        self.state.load(Ordering::Acquire) == WITNESS_ISSUED
    }
    /// The run asked for the witness and got none (refused, not ready in time, for another plan, or not asked
    /// successfully): no DLL loaded for that reason.
    pub fn refused(&self) -> bool {
        self.state.load(Ordering::Acquire) == WITNESS_REFUSED
    }
}
impl Witness for SeamWitness {
    fn ready(&self) -> Result<WitnessToken, String> {
        let answer = self.ask();
        self.state.store(
            if answer.is_ok() {
                WITNESS_ISSUED
            } else {
                WITNESS_REFUSED
            },
            Ordering::Release,
        );
        answer
    }

    fn recheck(&self, token: &WitnessToken) -> Result<(), String> {
        self.check_again(token)
    }
}
impl SeamWitness {
    fn ask(&self) -> Result<WitnessToken, String> {
        let Some(session) = self.seam.session() else {
            return Err("kiln-sfi never attached to kiln-eml's startup seam".into());
        };
        let key = self.seam.key(session);
        let mut out = KilnEmlStartupWitnessV1 {
            size: WITNESS_SIZE,
            state: 0,
            plan_id: [0; 16],
            revision: 0,
            flags: 0,
            reserved: 0,
        };
        let wait = self.seam.wait;
        let (result, text, _) = Seam::with_text(|buffer, capacity, required, again| {
            // SAFETY: a sized key and witness, a buffer of `capacity` bytes, a live required. The second call (after
            // BUFFER_TOO_SMALL) polls: the header guarantees it gives the same answer.
            unsafe {
                wait(
                    &raw const key,
                    if again { 0 } else { WITNESS_WAIT_MS },
                    &raw mut out,
                    buffer,
                    capacity,
                    required,
                )
            }
        });
        if result != KILN_OK {
            return Err(format!(
                "kiln-eml's startup witness could not be asked ({})",
                result_word(result)
            ));
        }
        match out.state as i32 {
            KILN_EML_WITNESS_READY if out.plan_id == self.seam.plan_id => Ok(WitnessToken {
                plan_id: out.plan_id,
                revision: out.revision,
            }),
            KILN_EML_WITNESS_READY => Err(format!(
                "kiln-eml's startup witness names another launch plan ({}, this start's is {})",
                uuid(&out.plan_id),
                uuid(&self.seam.plan_id)
            )),
            KILN_EML_WITNESS_REFUSED => Err(format!(
                "kiln-eml refused the startup witness: {}",
                text.as_deref().unwrap_or("it gave no reason")
            )),
            KILN_EML_WITNESS_PENDING => Err(format!(
                "kiln-eml's startup witness was not ready within {} ms",
                WITNESS_WAIT_MS
            )),
            other => Err(format!(
                "kiln-eml answered startup witness state {other}, which this kiln-sfi does not know"
            )),
        }
    }

    fn check_again(&self, token: &WitnessToken) -> Result<(), String> {
        let Some(session) = self.seam.session() else {
            return Err("kiln-sfi never attached to kiln-eml's startup seam".into());
        };
        let key = self.seam.key(session);
        let witness = KilnEmlStartupWitnessV1 {
            size: WITNESS_SIZE,
            state: KILN_EML_WITNESS_READY as u32,
            plan_id: token.plan_id,
            revision: token.revision,
            flags: 0,
            reserved: 0,
        };
        let recheck = self.seam.recheck;
        let (result, text, required) = Seam::with_text(|buffer, capacity, required, _| {
            // SAFETY: a sized key and witness, a buffer of `capacity` bytes, a live required.
            unsafe {
                recheck(
                    &raw const key,
                    &raw const witness,
                    buffer,
                    capacity,
                    required,
                )
            }
        });
        match result {
            KILN_OK if required == 0 => Ok(()),
            KILN_OK | KILN_ERR_BUFFER_TOO_SMALL => Err(format!(
                "kiln-eml says the input it witnessed changed: {}",
                text.as_deref().unwrap_or("its reason does not fit")
            )),
            // kiln-eml's wait for the terminal had ended: it ran its pass without this terminal, after its own check,
            // and a DLL that loaded after that check is seen by no one. Not confirmed, so not coherent (fail closed); a
            // run that loaded no DLL changed nothing (plan_source::incoherent).
            KILN_ERR_UNAVAILABLE => Err(RECHECK_GONE.to_owned()),
            other => Err(format!(
                "kiln-eml's recheck of the input it witnessed could not be asked ({})",
                result_word(other)
            )),
        }
    }
}

// ---- the native terminal (native-terminal.schema.json v1) ----

/// RFC 4122 bytes as canonical lower-case UUID text.
pub fn uuid(bytes: &[u8; 16]) -> String {
    let h = hex(bytes);
    format!(
        "{}-{}-{}-{}-{}",
        &h[0..8],
        &h[8..12],
        &h[12..16],
        &h[16..20],
        &h[20..32]
    )
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
/// `text` cut to `max` characters at a character boundary (the schemas count code points).
fn clamp(text: &str, max: usize) -> &str {
    match text.char_indices().nth(max) {
        Some((at, _)) => &text[..at],
        None => text,
    }
}
fn string(out: &mut String, text: &str) {
    out.push_str(&serde_json::to_string(text).unwrap_or_else(|_| "\"\"".into()));
}
/// A required free text (minLength 1), clamped.
fn text_value(out: &mut String, text: &str) {
    let text = clamp(text.trim(), TEXT_MAX);
    string(
        out,
        if text.is_empty() {
            "no reason was given"
        } else {
            text
        },
    );
}

/// The terminal's word for an outcome (the native manager's `Outcome` kinds).
pub fn outcome_word(outcome: &Outcome) -> &'static str {
    match outcome {
        Outcome::Loaded { .. } => "loaded",
        Outcome::ForeignOwned { .. } => "foreign-owned",
        Outcome::Refused(_) => "refused",
        Outcome::Faulted(_) => "faulted",
        Outcome::TimedOut(_) => "timed-out",
        Outcome::Withdrawn(_) => "withdrawn",
    }
}
fn outcome_detail(outcome: &Outcome) -> Option<&str> {
    match outcome {
        Outcome::Refused(why)
        | Outcome::Faulted(why)
        | Outcome::TimedOut(why)
        | Outcome::Withdrawn(why) => Some(why),
        Outcome::Loaded { .. } | Outcome::ForeignOwned { .. } => None,
    }
}

/// The fixed words of a withdrawn package's `code` in the terminal (sfi-state's code grammar):
/// - `native-policy-refused`: Kiln's native.policy refuses package DLLs this start;
/// - `native-not-armed`: the loader was not armed for another reason (the plan lists no DLL, the loader thread could
///   not start, the plan's native part could not be read);
/// - `witness-refused`: an armed run whose startup witness was refused, late or for another plan;
/// - `dll-faulted`, `dll-timed-out`, `dll-refused`: the package's first DLL that did not load, by its outcome (also an
///   armed run whose loader stopped before it asked for the witness: its DLLs are refused with the loader's reason);
/// - `dependency-withdrawn`: a package it depends on was withdrawn (Lua-only packages too).
pub const WITHDRAWN_CODES: [&str; 7] = [
    "native-policy-refused",
    "native-not-armed",
    "witness-refused",
    "dll-faulted",
    "dll-timed-out",
    "dll-refused",
    "dependency-withdrawn",
];

/// The native terminal as native-terminal.schema.json v1, its fields in the schema's order. `policy`: the native
/// policy's refusal this start (`NativePermission::refusal`); `witness_refused`: the armed run asked for the startup
/// witness and got none ([`SeamWitness::refused`]).
pub fn terminal_json(
    plan_id: [u8; 16],
    terminal: &NativeTerminal,
    policy: Option<&str>,
    witness_refused: bool,
) -> String {
    let mut out = String::from("{\"format\":1,\"planId\":");
    string(&mut out, &uuid(&plan_id));
    out.push_str(&format!(
        ",\"notArmed\":{},\"deadlinePassed\":{},\"coherent\":",
        terminal.not_armed.is_some(),
        terminal.deadline_passed
    ));
    match &terminal.coherent {
        Ok(()) => out.push_str("{\"ok\":true}"),
        Err(why) => {
            out.push_str("{\"ok\":false,\"text\":");
            text_value(&mut out, why);
            out.push('}');
        }
    }
    out.push_str(",\"withdrawn\":[");
    for (i, w) in terminal.withdrawn.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        let failed = terminal
            .records
            .iter()
            .find(|r| r.mod_id == w.mod_id && !r.outcome.admitted());
        let code = if !w.caused_by.is_empty() {
            "dependency-withdrawn"
        } else if let Some(reason) = &terminal.not_armed {
            if Some(reason.as_str()) == policy {
                "native-policy-refused"
            } else {
                "native-not-armed"
            }
        } else if witness_refused {
            "witness-refused"
        } else {
            match failed.map(|r| &r.outcome) {
                Some(Outcome::Faulted(_)) => "dll-faulted",
                Some(Outcome::TimedOut(_)) => "dll-timed-out",
                _ => "dll-refused",
            }
        };
        let text = match failed.and_then(|r| outcome_detail(&r.outcome)) {
            Some(detail) if w.caused_by.is_empty() && detail != w.reason => {
                format!("{} ({detail})", w.reason)
            }
            _ => w.reason.clone(),
        };
        out.push_str("{\"id\":");
        string(&mut out, &w.mod_id);
        out.push_str(",\"code\":");
        string(&mut out, code);
        out.push_str(",\"text\":");
        text_value(&mut out, &text);
        out.push_str(",\"chain\":[");
        for (j, id) in w.caused_by.iter().take(CHAIN_MAX).enumerate() {
            if j > 0 {
                out.push(',');
            }
            string(&mut out, id);
        }
        out.push_str("]}");
    }
    out.push_str("],\"dlls\":[");
    for (i, record) in terminal.records.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str("{\"statusId\":");
        string(&mut out, &record.status_id);
        out.push_str(",\"modId\":");
        string(&mut out, &record.mod_id);
        out.push_str(",\"outcome\":");
        string(&mut out, outcome_word(&record.outcome));
        out.push('}');
    }
    out.push_str("]}");
    out
}

/// What the launch plan says about each candidate's native code, for the terminal of a run never armed (kept from the
/// early phase, so the push needs no plan): the native plan lists planned DLLs only, but admission's native verdict
/// also refuses a package that registers DLLs from its Lua (`runtime-register-dll`) when no loader is armed
/// ([`crate::plan_source::native_verdict`]). kiln-eml must not apply startup data for a package kiln-sfi will not run.
#[derive(Clone, Debug, Default)]
pub struct NativeFootprint {
    /// The plan's load order: id, whether it is a candidate that ships native code (a planned DLL, or
    /// `runtime-register-dll`), its included dependencies.
    packages: Vec<(String, bool, Vec<String>)>,
}
impl NativeFootprint {
    pub fn of(plan: &LaunchPlan) -> Self {
        let packages = plan
            .load_order()
            .into_iter()
            .filter_map(|id| plan.package(id))
            .map(|p| {
                (
                    p.id.to_owned(),
                    p.decision == Decision::Candidate && p.has_native(),
                    p.dependencies
                        .iter()
                        .filter(|d| d.included)
                        .map(|d| d.id.to_owned())
                        .collect(),
                )
            })
            .collect();
        Self { packages }
    }

    /// The packages `terminal` withdraws, as kiln-eml must learn them. An armed run's are its own. A run never armed
    /// withdraws every candidate that ships native code (the run's own words for one with a planned DLL, admission's
    /// for one that only registers DLLs from its Lua), then every package that depends on one, nearest first, in load
    /// order.
    pub fn withdrawn(&self, terminal: &NativeTerminal) -> Vec<Withdrawn> {
        let Some(reason) = &terminal.not_armed else {
            return terminal.withdrawn.clone();
        };
        let mut out: Vec<Withdrawn> = Vec::new();
        for (id, native, dependencies) in &self.packages {
            let own = terminal
                .withdrawn
                .iter()
                .find(|w| &w.mod_id == id && w.caused_by.is_empty());
            if *native || own.is_some() {
                out.push(own.cloned().unwrap_or_else(|| Withdrawn {
                    mod_id: id.clone(),
                    reason: crate::plan_source::registers_without_loader(reason),
                    caused_by: Vec::new(),
                }));
                continue;
            }
            if let Some(dep) = dependencies
                .iter()
                .find_map(|d| out.iter().find(|w| &w.mod_id == d))
            {
                let mut chain = vec![dep.mod_id.clone()];
                chain.extend(dep.caused_by.iter().cloned());
                let reason = format!("depends on withdrawn package {}", dep.mod_id);
                out.push(Withdrawn {
                    mod_id: id.clone(),
                    reason,
                    caused_by: chain,
                });
            }
        }
        out
    }
}

// ---- the outcome (startup-outcome.schema.json v1) ----

/// What kiln-eml's startup outcome gives this start: admission's verdict, sfi.json's rows and, when the live
/// generation is proven, the cross-checked archive identity to grant. Without a grant, `refusal` says why (sfi.json's
/// refused code and the text): Kiln's interpreter does not run without one.
#[derive(Clone)]
pub struct StartupRead {
    pub verdict: StartupVerdict,
    pub rows: Vec<StartupRow>,
    pub grant: Option<KilnEmlArchiveIdentityV1>,
    pub refusal: Option<(&'static str, String)>,
}
impl std::fmt::Debug for StartupRead {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("StartupRead")
            .field("verdict", &self.verdict)
            .field("rows", &self.rows)
            .field(
                "grant",
                &self
                    .grant
                    .map(|g| (g.generation, hex(&g.archive_sha256), uuid(&g.launch_id))),
            )
            .field("refusal", &self.refusal)
            .finish()
    }
}
impl Default for StartupRead {
    fn default() -> Self {
        Self::not_built()
    }
}
/// The text of a start without a grant.
pub const NO_SEAM: &str = "kiln-eml has no startup seam (it is older than Kiln's interpreter), so no startup pass granted Kiln's interpreter an archive generation";
impl StartupRead {
    /// No seam (an older kiln-eml).
    pub fn not_built() -> Self {
        Self {
            verdict: StartupVerdict::NotBuilt,
            rows: Vec::new(),
            grant: None,
            refusal: Some(("startup-grant-missing", NO_SEAM.to_owned())),
        }
    }
    /// No usable outcome: `code` and why.
    pub fn missing(code: &'static str, text: impl Into<String>) -> Self {
        let text = text.into();
        Self {
            verdict: StartupVerdict::Missing {
                code: code.to_owned(),
                text: text.clone(),
            },
            rows: Vec::new(),
            grant: None,
            refusal: Some((code, text)),
        }
    }
}

/// The base origins a receipt records (kiln-eml's inspection.rs ORIGINS, as startup-outcome.schema.json pins them).
const BASE_ORIGINS: [(&str, &str); 2] = [
    (
        "first-managed-unclassified",
        "unclassified: game files as found when Kiln first managed this install",
    ),
    (
        "rebaselined-unclassified",
        "unclassified: game files as found when Kiln took a new baseline after the game changed",
    ),
];

/// The launch kiln-sfi knows (kiln.launch and the plan it read).
struct Launch {
    launch_id: u64,
    plan_id: [u8; 16],
    plan_sha256: [u8; 32],
}

enum Rejected {
    /// Another launch's record, or an identity that does not match the record or this launch.
    Mismatch(String),
    /// Not a startup-outcome.schema.json v1 record.
    Invalid(String),
}
fn invalid(why: impl Into<String>) -> Rejected {
    Rejected::Invalid(why.into())
}

fn closed<'a>(
    value: &'a Value,
    what: &str,
    required: &[&str],
    optional: &[&str],
) -> Result<&'a Map<String, Value>, Rejected> {
    let map = value
        .as_object()
        .ok_or_else(|| invalid(format!("{what} is not an object")))?;
    if let Some(key) = map
        .keys()
        .find(|k| !required.contains(&k.as_str()) && !optional.contains(&k.as_str()))
    {
        return Err(invalid(format!("{what} has an unknown member {key:?}")));
    }
    if let Some(key) = required.iter().find(|k| !map.contains_key(**k)) {
        return Err(invalid(format!("{what} lacks {key}")));
    }
    Ok(map)
}
fn word<'a>(map: &'a Map<String, Value>, key: &str, allowed: &[&str]) -> Result<&'a str, Rejected> {
    map[key]
        .as_str()
        .filter(|w| allowed.contains(w))
        .ok_or_else(|| invalid(format!("{key} is not one of {allowed:?}")))
}
fn boolean(map: &Map<String, Value>, key: &str) -> Result<bool, Rejected> {
    map[key]
        .as_bool()
        .ok_or_else(|| invalid(format!("{key} is not a boolean")))
}
fn lower_hex(text: &str, len: usize) -> bool {
    text.len() == len
        && text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn sha256(value: &Value, key: &str) -> Result<Option<[u8; 32]>, Rejected> {
    match value {
        Value::Null => Ok(None),
        Value::String(text) if lower_hex(text, 64) => {
            let mut out = [0u8; 32];
            for (i, byte) in out.iter_mut().enumerate() {
                *byte = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16)
                    .map_err(|_| invalid(format!("{key} is not a SHA-256")))?;
            }
            Ok(Some(out))
        }
        _ => Err(invalid(format!(
            "{key} is not a lower-case SHA-256 or null"
        ))),
    }
}
fn code_word(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= 64
        && text
            .bytes()
            .next()
            .is_some_and(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
        && text
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'.' || b == b'-')
}
fn free_text(value: &Value, what: &str, max: usize) -> Result<String, Rejected> {
    match value.as_str() {
        Some(text) if !text.is_empty() && text.chars().count() <= max => Ok(text.to_owned()),
        _ => Err(invalid(format!(
            "{what} is not a text of 1 to {max} characters"
        ))),
    }
}
fn optional_text(value: &Value, what: &str, max: usize) -> Result<Option<String>, Rejected> {
    match value {
        Value::Null => Ok(None),
        other => free_text(other, what, max).map(Some),
    }
}

/// Reads `json` strictly against startup-outcome.schema.json v1 and cross-checks it, and `identity`, against
/// `launch`.
fn parse_outcome(
    json: &[u8],
    identity: &KilnEmlArchiveIdentityV1,
    launch: &Launch,
    known: &dyn Fn(&str) -> bool,
) -> Result<StartupRead, Rejected> {
    let doc: Value =
        serde_json::from_slice(json).map_err(|e| invalid(format!("it does not parse: {e}")))?;
    let top = closed(
        &doc,
        "the outcome",
        &[
            "format",
            "launchId",
            "planId",
            "planSha256",
            "dialect",
            "state",
            "grant",
            "receiptSha256",
            "archiveSha256",
            "baseOrigin",
            "native",
            "attempt",
            "error",
            "retryError",
            "packages",
            "warnings",
        ],
        &[],
    )?;
    if top["format"].as_u64() != Some(1) {
        return Err(invalid("format is not 1"));
    }
    // the launch it names: this one, or the record is another launch's
    let launch_text = top["launchId"]
        .as_str()
        .filter(|t| lower_hex(t, 16))
        .ok_or_else(|| invalid("launchId is not 16 lower-case hex digits"))?;
    if launch_text != format!("{:016x}", launch.launch_id) {
        return Err(Rejected::Mismatch(format!(
            "it names launch {launch_text}, this start is {:016x}",
            launch.launch_id
        )));
    }
    let plan_text = top["planId"]
        .as_str()
        .ok_or_else(|| invalid("planId is not a text"))?;
    if plan_text != uuid(&launch.plan_id) {
        return Err(Rejected::Mismatch(format!(
            "it names plan {plan_text}, this start's plan is {}",
            uuid(&launch.plan_id)
        )));
    }
    match sha256(&top["planSha256"], "planSha256")? {
        Some(hash) if hash == launch.plan_sha256 => {}
        Some(_) => {
            return Err(Rejected::Mismatch(
                "its plan hash is not the one kiln.launch froze".into(),
            ));
        }
        None => return Err(invalid("planSha256 is null")),
    }
    let dialect = word(top, "dialect", &["shroudforge", "eml", "none"])?;
    let state = word(
        top,
        "state",
        &[
            "committed",
            "cached",
            "unchanged",
            "restored",
            "refused",
            "failed",
        ],
    )?;
    let grant = boolean(top, "grant")?;
    let proven = matches!(state, "committed" | "cached" | "unchanged" | "restored");
    if grant != proven {
        return Err(invalid(format!("grant is {grant} for state {state}")));
    }
    let receipt = sha256(&top["receiptSha256"], "receiptSha256")?;
    let archive = sha256(&top["archiveSha256"], "archiveSha256")?;
    if grant && archive.is_none() {
        return Err(invalid("a grant without archiveSha256"));
    }
    if !grant && (archive.is_some() || receipt.is_some()) {
        return Err(invalid("digests without a grant"));
    }
    match &top["baseOrigin"] {
        Value::Null => {}
        origin => {
            let origin = closed(origin, "baseOrigin", &["token", "label"], &[])?;
            let pair = (origin["token"].as_str(), origin["label"].as_str());
            if !BASE_ORIGINS
                .iter()
                .any(|(token, label)| pair == (Some(*token), Some(*label)))
            {
                return Err(invalid("baseOrigin is not one of inspection.rs's origins"));
            }
            if state == "restored" {
                return Err(invalid("a restored start keeps no base origin"));
            }
        }
    }
    if state == "restored" && receipt.is_some() {
        return Err(invalid("a restored start keeps no receipt"));
    }
    let native = closed(
        &top["native"],
        "native",
        &["received", "coherent"],
        &["text"],
    )?;
    let (received, coherent) = (boolean(native, "received")?, boolean(native, "coherent")?);
    if !received && coherent {
        return Err(invalid("native: no terminal is never coherent"));
    }
    match (coherent, native.get("text")) {
        (true, None) => {}
        (false, Some(text)) => {
            free_text(text, "native.text", TEXT_MAX)?;
        }
        _ => {
            return Err(invalid(
                "native.text is there exactly when it is not coherent",
            ));
        }
    }
    let attempt = word(top, "attempt", &["first", "retry", "none"])?;
    let error = optional_text(&top["error"], "error", 65536)?;
    let retry_error = optional_text(&top["retryError"], "retryError", 65536)?;
    match attempt {
        "retry" if dialect != "shroudforge" || error.is_none() => {
            return Err(invalid(
                "a retry is ShroudForge's and carries the first error",
            ));
        }
        "first" if retry_error.is_some() => {
            return Err(invalid("a first attempt has no retryError"));
        }
        "none" if error.is_some() || retry_error.is_some() => {
            return Err(invalid("no attempt has no error"));
        }
        _ => {}
    }
    let warnings = top["warnings"]
        .as_array()
        .ok_or_else(|| invalid("warnings is not an array"))?;
    if warnings.len() > WARNINGS_MAX {
        return Err(invalid("more than 64 warnings"));
    }
    for warning in warnings {
        let warning = closed(warning, "a warning", &["code", "text"], &[])?;
        if !warning["code"].as_str().is_some_and(code_word) {
            return Err(invalid("a warning's code is not a code word"));
        }
        free_text(&warning["text"], "a warning's text", 8192)?;
    }
    let packages = top["packages"]
        .as_array()
        .ok_or_else(|| invalid("packages is not an array"))?;
    if packages.len() > ROWS_MAX {
        return Err(invalid("more than 4096 package rows"));
    }
    if dialect == "none" && !packages.is_empty() {
        return Err(invalid("dialect none has no package rows"));
    }
    let mut rows = Vec::with_capacity(packages.len());
    let mut seen = BTreeSet::new();
    for item in packages {
        let row = parse_row(item, dialect, grant, known)?;
        if !seen.insert(row.id.clone()) {
            return Err(invalid(format!("package {} has two rows", row.id)));
        }
        rows.push(row);
    }
    check_identity(identity, grant, archive, launch)?;
    let mut committed = BTreeSet::new();
    let mut refused = BTreeMap::new();
    for row in &rows {
        if row.data.present() {
            committed.insert(row.id.clone());
        } else {
            refused.insert(
                row.id.clone(),
                Refusal {
                    code: row
                        .code
                        .clone()
                        .unwrap_or_else(|| "startup-not-committed".into()),
                    text: row
                        .text
                        .clone()
                        .unwrap_or_else(|| "its startup data was not applied this start".into()),
                    chain: row.chain.clone(),
                },
            );
        }
    }
    let refusal = (!grant).then(|| {
        let why = error
            .as_deref()
            .map(|e| format!(": {}", clamp(e.lines().next().unwrap_or(e), 400)))
            .unwrap_or_default();
        (
            "startup-grant-missing",
            format!(
                "kiln-eml's startup pass granted no archive generation this start (state {state}{why})"
            ),
        )
    });
    Ok(StartupRead {
        verdict: StartupVerdict::Committed {
            packages: committed,
            refused,
        },
        rows,
        grant: grant.then_some(*identity),
        refusal,
    })
}

fn parse_row(
    item: &Value,
    dialect: &str,
    grant: bool,
    known: &dyn Fn(&str) -> bool,
) -> Result<StartupRow, Rejected> {
    let row = closed(
        item,
        "a package row",
        &["id", "version", "pass", "data", "archivePresent", "chain"],
        &["code", "text"],
    )?;
    let id = row["id"]
        .as_str()
        .filter(|id| sf_id(id))
        .ok_or_else(|| invalid("a row's id is not a ShroudForge identifier"))?
        .to_owned();
    if !known(&id) {
        return Err(Rejected::Mismatch(format!(
            "it has a row for {id}, which this start's plan does not list"
        )));
    }
    let what = |rule: &str| invalid(format!("row {id}: {rule}"));
    let version = match &row["version"] {
        Value::Null => None,
        Value::String(v) if (1..=64).contains(&v.chars().count()) => Some(v.clone()),
        _ => return Err(what("version is not a text of 1 to 64 characters or null")),
    };
    if row["pass"].as_str() != Some(dialect) {
        return Err(what("its pass is not the outcome's dialect"));
    }
    let data = match row["data"].as_str() {
        Some("committed") => Data::Committed,
        Some("cached-verified") => Data::CachedVerified,
        Some("failed") => Data::Failed,
        Some("excluded") => Data::Excluded,
        _ => {
            return Err(what(
                "data is not committed, cached-verified, failed or excluded",
            ));
        }
    };
    let archive_present = match &row["archivePresent"] {
        Value::Null => None,
        Value::Bool(b) => Some(*b),
        _ => return Err(what("archivePresent is not a boolean or null")),
    };
    let code = match row.get("code") {
        None => None,
        Some(Value::String(c)) if code_word(c) => Some(c.clone()),
        Some(_) => return Err(what("code is not a code word")),
    };
    let text = match row.get("text") {
        None => None,
        Some(t) => Some(free_text(t, "a row's text", TEXT_MAX)?),
    };
    if code.is_some() != text.is_some() {
        return Err(what("code and text come together"));
    }
    let chain: Vec<String> = row["chain"]
        .as_array()
        .ok_or_else(|| what("chain is not an array"))?
        .iter()
        .map(|c| c.as_str().filter(|c| sf_id(c)).map(str::to_owned))
        .collect::<Option<_>>()
        .ok_or_else(|| what("a chain entry is not a ShroudForge identifier"))?;
    if chain.len() > CHAIN_MAX || chain.contains(&id) {
        return Err(what("its chain is longer than 64 or names itself"));
    }
    let code_is = |c: &str| code.as_deref() == Some(c);
    let ok = match data {
        Data::Committed | Data::CachedVerified => {
            archive_present == Some(true) && code.is_none() && chain.is_empty()
        }
        Data::Failed => {
            code_is("startup-failed") && archive_present != Some(true) && chain.is_empty()
        }
        _ => {
            code.is_some()
                && archive_present != Some(true)
                && !code_is("startup-failed")
                && (!code_is("dependency-skipped") || !chain.is_empty())
                && (!code_is("startup-retry-failed") || chain.is_empty())
        }
    };
    if !ok {
        return Err(what("its data, code, chain and archivePresent disagree"));
    }
    if !grant && archive_present.is_some() {
        return Err(what("archivePresent is known without a grant"));
    }
    if grant
        && (data == Data::Failed || code_is("dependency-skipped"))
        && archive_present != Some(false)
    {
        return Err(what(
            "a failed or dependency-skipped row is proven absent with a grant",
        ));
    }
    Ok(StartupRow {
        id,
        version,
        data,
        archive_present,
        code,
        text,
        chain,
    })
}

/// The identity `outcome` filled, against the JSON and this launch.
fn check_identity(
    identity: &KilnEmlArchiveIdentityV1,
    grant: bool,
    archive: Option<[u8; 32]>,
    launch: &Launch,
) -> Result<(), Rejected> {
    let mismatch = |why: &str| Err(Rejected::Mismatch(format!("its archive identity {why}")));
    if identity.size != IDENTITY_SIZE {
        return mismatch("changed its size");
    }
    if !grant {
        let zero = identity.phase == 0
            && identity.launch_id == [0; 16]
            && identity.plan_id == [0; 16]
            && identity.generation == 0
            && identity.reload_epoch == 0
            && identity.plan_sha256 == [0; 32]
            && identity.archive_sha256 == [0; 32]
            && identity.registry_id == 0
            && identity.registry_sha256 == [0; 32];
        return if zero {
            Ok(())
        } else {
            mismatch("is not zero for an outcome without a grant")
        };
    }
    if identity.phase != KILN_EML_ARCHIVE_PUBLISHED as u32 {
        return mismatch("is not PUBLISHED");
    }
    let mut launch_id = [0u8; 16];
    launch_id[8..].copy_from_slice(&launch.launch_id.to_be_bytes());
    if identity.launch_id != launch_id {
        return mismatch("names another launch id");
    }
    if identity.plan_id != launch.plan_id {
        return mismatch("names another plan id");
    }
    if identity.plan_sha256 != launch.plan_sha256 {
        return mismatch("names another plan hash");
    }
    if Some(identity.archive_sha256) != archive {
        return mismatch("has another archive digest than the outcome's archiveSha256");
    }
    if identity.generation == 0 {
        return mismatch("has generation 0");
    }
    Ok(())
}
