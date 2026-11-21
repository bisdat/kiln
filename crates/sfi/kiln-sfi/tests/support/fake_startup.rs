// SPDX-License-Identifier: MIT
//! A fake kiln.eml.startup v1 provider (`native/core/abi/services/kiln_eml_startup.h`), built from kiln-abi's generated
//! types the way fake_archive.rs fakes kiln.eml.archive. Never a real kiln-eml: tests reach it through
//! `Boundary::with_startup(fake_startup::resolve)`.
//!
//! - [`getter`] serves [`TABLE`] for `min_version` 1 and NULL above, as kiln-eml's early export.
//! - What one start's kiln-eml answers is a [`Config`] the test installs on the thread that enters kiln-sfi's early
//!   phase ([`install`]): the attach on that thread takes it and issues a fresh session, so tests run in parallel.
//!   Every other member finds its start by the key's session (they run on the loader thread or the boot thread).
//! - The header's rules: key and record sizes, zero flags, the key's launch id and plan hash as attach saw them, the
//!   issued session, `required` mandatory, BUFFER_TOO_SMALL with the complete size and nothing else written, one
//!   attach and one terminal (a terminal whose `dlls` are not `planned_dlls` in number is INVALID_ARGUMENT), a
//!   recheck only of the READY witness issued.
//! - What it saw ([`seen`]): every call in order, the timeouts `witness_wait` got, the terminal's JSON, the identity
//!   `outcome` served.
#![allow(dead_code)]
use kiln_abi::services::kiln_eml_startup::*;
use kiln_abi::*;
use serde_json::{Value, json};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_char;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, LazyLock, Mutex};

/// What `witness_wait` answers.
#[derive(Clone, Debug)]
pub enum WitnessAnswer {
    /// READY for the config's plan id, with this revision.
    Ready(u64),
    /// READY for another plan id.
    ReadyFor([u8; 16], u64),
    /// REFUSED with this reason.
    Refused(String),
    /// PENDING: the timeout passed (the fake does not sleep; the timeout it was given is recorded).
    Pending,
    /// This result.
    Fail(KilnResult),
}
/// What `witness_recheck` answers.
#[derive(Clone, Debug)]
pub enum RecheckAnswer {
    Unchanged,
    Changed(String),
    Fail(KilnResult),
}
/// An edit of a built outcome record and the identity served with it (a mismatch, a broken rule).
pub type Edit = fn(&mut Value, &mut KilnEmlArchiveIdentityV1);
/// What `outcome` answers.
#[derive(Clone, Debug)]
pub enum OutcomeAnswer {
    /// UNAVAILABLE (the barrier did not finish).
    Unavailable,
    /// A record for this start's key and plan: a grant exactly when `state` proves the live generation.
    Record {
        state: &'static str,
        dialect: String,
        rows: Vec<Value>,
        edit: Option<Edit>,
    },
}

/// One start's kiln-eml.
#[derive(Clone, Debug)]
pub struct Config {
    /// What attach answers (OK issues the session).
    pub attach: KilnResult,
    /// The launch plan's id (READY's `plan_id`, the outcome's `planId` and identity).
    pub plan_id: [u8; 16],
    pub witness: WitnessAnswer,
    pub recheck: RecheckAnswer,
    /// What `native_terminal` answers to the first valid push.
    pub terminal: KilnResult,
    pub outcome: OutcomeAnswer,
}
impl Config {
    /// A kiln-eml that witnesses, takes the terminal and grants this plan: every candidate with startup data committed
    /// (state `committed`, or `unchanged` without one).
    pub fn granting(plan: &Value) -> Self {
        let dialect = plan["startupDialect"].as_str().unwrap_or("none").to_owned();
        let rows: Vec<Value> = if dialect == "none" {
            Vec::new()
        } else {
            plan["packages"]
                .as_array()
                .into_iter()
                .flatten()
                .filter(|p| p["decision"] == "candidate" && p["data"]["required"] == true)
                .map(|p| committed(p["id"].as_str().unwrap(), &dialect))
                .collect()
        };
        Self {
            attach: KILN_OK,
            plan_id: plan_id(plan),
            witness: WitnessAnswer::Ready(7),
            recheck: RecheckAnswer::Unchanged,
            terminal: KILN_OK,
            outcome: OutcomeAnswer::Record {
                state: if rows.is_empty() {
                    "unchanged"
                } else {
                    "committed"
                },
                dialect,
                rows,
                edit: None,
            },
        }
    }
    /// [`Self::granting`] for the plan this thread's fake core serves (support's plan file).
    pub fn granting_current() -> Self {
        let bytes = std::fs::read(crate::support::plan_path()).expect("the plan file");
        Self::granting(&serde_json::from_slice(&bytes).expect("a plan"))
    }
    /// The same start with another outcome.
    pub fn with_outcome(mut self, outcome: OutcomeAnswer) -> Self {
        self.outcome = outcome;
        self
    }
}

/// The plan id bytes of a plan document (`plan_id`, canonical UUID text).
pub fn plan_id(plan: &Value) -> [u8; 16] {
    let text: String = plan["plan_id"].as_str().expect("plan_id").replace('-', "");
    let mut id = [0u8; 16];
    for (i, byte) in id.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap();
    }
    id
}

/// A committed row.
pub fn committed(id: &str, pass: &str) -> Value {
    json!({"id": id, "version": "1.0.0", "pass": pass, "data": "committed", "archivePresent": true, "chain": []})
}
/// A row whose data is not in the live generation.
pub fn left_out(
    id: &str,
    pass: &str,
    data: &str,
    present: Value,
    code: &str,
    text: &str,
    chain: &[&str],
) -> Value {
    json!({"id": id, "version": "1.0.0", "pass": pass, "data": data, "archivePresent": present,
        "code": code, "text": text, "chain": chain})
}

/// The identity a grant serves for (launch id, plan id, plan hash): fake_archive's fixture generation (7), registry (3)
/// and digests, epoch 1, the launch id as kiln-eml maps it (8 zero bytes, then the u64 big-endian).
pub fn granted_identity(
    launch_id: u64,
    plan_id: [u8; 16],
    plan_sha256: [u8; 32],
) -> KilnEmlArchiveIdentityV1 {
    let mut launch = [0u8; 16];
    launch[8..].copy_from_slice(&launch_id.to_be_bytes());
    KilnEmlArchiveIdentityV1 {
        size: std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32,
        phase: KILN_EML_ARCHIVE_PUBLISHED as u32,
        launch_id: launch,
        plan_id,
        generation: 7,
        reload_epoch: 1,
        plan_sha256,
        archive_sha256: [0x22; 32],
        registry_id: 3,
        registry_sha256: [0x33; 32],
    }
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn uuid(bytes: &[u8; 16]) -> String {
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
/// The game version and registry version the empty registry of a grant carries.
pub const GAME: &str = "1.0.4-startup";
pub const REGISTRY: &str = "registry rev S";
/// An empty archive registry whose pages are pinned to `identity`.
pub fn pages(identity: KilnEmlArchiveIdentityV1) -> crate::fake_archive::Pages {
    Arc::new(move |first, _| {
        let tagged = |v: u64| format!("u64:{v:016x}");
        serde_json::to_vec(&json!({
            "schema_version": 1,
            "pin": {
                "launch_id": uuid(&identity.launch_id),
                "plan_id": uuid(&identity.plan_id),
                "generation": tagged(identity.generation),
                "reload_epoch": tagged(identity.reload_epoch),
                "plan_sha256": hex(&identity.plan_sha256),
                "archive_sha256": hex(&identity.archive_sha256),
                "registry_id": tagged(identity.registry_id),
                "registry_sha256": hex(&identity.registry_sha256),
            },
            "game_version": GAME,
            "registry_version": REGISTRY,
            "count": tagged(0),
            "first": tagged(first),
            "types": [],
        }))
        .unwrap()
    })
}

/// What the fake saw for one start.
#[derive(Clone, Debug, Default)]
pub struct Seen {
    /// "attach(<planned>)", "witness_wait(<timeout>)", "witness_recheck", "native_terminal", "outcome(<capacity>)".
    pub calls: Vec<String>,
    pub launch_id: u64,
    pub plan_sha256: [u8; 32],
    pub planned_dlls: u32,
    pub session: u64,
    /// The first terminal pushed (what kiln-eml would use), as text.
    pub terminal: Option<String>,
    /// When the attach and the first terminal push came, and the name of the thread that pushed it.
    pub attached_at: Option<std::time::Instant>,
    pub pushed_at: Option<std::time::Instant>,
    pub pushed_on: Option<String>,
    /// The identity `outcome` filled with OK.
    pub served: Option<[u8; 160]>,
}
impl Seen {
    pub fn count(&self, call: &str) -> usize {
        self.calls.iter().filter(|c| c.starts_with(call)).count()
    }
    /// The terminal as JSON.
    pub fn terminal_json(&self) -> Option<Value> {
        self.terminal
            .as_ref()
            .map(|t| serde_json::from_str(t).expect("the terminal is JSON"))
    }
}

struct Start {
    config: Config,
    seen: Seen,
    /// The READY witness issued (plan id, revision).
    issued: Option<([u8; 16], u64)>,
}
static STARTS: LazyLock<Mutex<HashMap<u64, Start>>> = LazyLock::new(Default::default);
static SESSIONS: AtomicU64 = AtomicU64::new(0x5e55_0000);
thread_local! {
    static PENDING: RefCell<Option<Config>> = const { RefCell::new(None) };
    static LAST: Cell<u64> = const { Cell::new(0) };
}

/// The next attach on this thread takes `config` (a start of its own).
pub fn install(config: Config) {
    PENDING.set(Some(config));
}
/// No start pending on this thread: an attach is UNAVAILABLE.
pub fn uninstall() {
    PENDING.set(None);
}
/// A `Boundary::with_startup` resolver: kiln-eml's export.
pub fn resolve() -> Option<kiln_sfi::startup_seam::Getter> {
    Some(getter)
}
/// A resolver for a kiln-eml without the seam.
pub fn none() -> Option<kiln_sfi::startup_seam::Getter> {
    None
}
/// What the start attached on this thread saw (`None`: no attach on this thread yet).
pub fn seen() -> Option<Seen> {
    let session = LAST.get();
    STARTS.lock().unwrap().get(&session).map(|s| s.seen.clone())
}
/// The identity the start attached on this thread served with its outcome.
pub fn served() -> Option<KilnEmlArchiveIdentityV1> {
    seen()?.served.map(|bytes| {
        // SAFETY: 160 bytes of an identity record (integers and byte arrays).
        unsafe { std::mem::transmute::<[u8; 160], KilnEmlArchiveIdentityV1>(bytes) }
    })
}
/// fake_archive grants `owner` the identity this thread's start served, with an empty registry pinned to it (so
/// the worker's runtime-create can read `game.types` from it). False when no identity was served.
pub fn grant_archive(owner: KilnOwner) -> bool {
    let Some(identity) = served().filter(|i| i.phase == KILN_EML_ARCHIVE_PUBLISHED as u32) else {
        return false;
    };
    crate::fake_archive::grant(
        owner,
        crate::fake_archive::Grant::new(identity, pages(identity)),
    );
    true
}

const KEY_SIZE: u32 = std::mem::size_of::<KilnEmlStartupKeyV1>() as u32;
const WITNESS_SIZE: u32 = std::mem::size_of::<KilnEmlStartupWitnessV1>() as u32;
const IDENTITY_SIZE: u32 = std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;

/// The key's record rules; the session's start.
fn start_of(key: *const KilnEmlStartupKeyV1) -> Result<u64, KilnResult> {
    if key.is_null() {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    }
    // SAFETY: the caller's key, its size first.
    let key = unsafe { *key };
    if key.size < KEY_SIZE || key.flags != 0 {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    }
    let starts = STARTS.lock().unwrap();
    let Some(start) = starts.get(&key.session) else {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    };
    if start.seen.launch_id != key.launch_id || start.seen.plan_sha256 != key.plan_sha256 {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    }
    Ok(key.session)
}
fn note(session: u64, call: String) {
    if let Some(start) = STARTS.lock().unwrap().get_mut(&session) {
        start.seen.calls.push(call);
    }
}
fn config(session: u64) -> Config {
    STARTS.lock().unwrap()[&session].config.clone()
}
/// Counted text into the caller's buffer: BUFFER_TOO_SMALL (with the complete size, nothing written) or OK.
fn write_text(
    text: &[u8],
    buffer: *mut c_char,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    // SAFETY: the caller's required.
    unsafe { required.write(text.len()) };
    if capacity < text.len() {
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    // SAFETY: `buffer` holds `capacity` >= len bytes.
    unsafe { std::ptr::copy_nonoverlapping(text.as_ptr(), buffer.cast::<u8>(), text.len()) };
    KILN_OK
}

unsafe extern "C" fn attach(
    key: *const KilnEmlStartupKeyV1,
    planned: u32,
    session: *mut u64,
) -> KilnResult {
    if key.is_null() || session.is_null() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    // SAFETY: the caller's key.
    let k = unsafe { *key };
    if k.size < KEY_SIZE || k.flags != 0 || k.session != 0 || k.launch_id == 0 {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let Some(config) = PENDING.take() else {
        return KILN_ERR_UNAVAILABLE;
    };
    if config.attach != KILN_OK {
        return config.attach;
    }
    let issued = SESSIONS.fetch_add(1, Ordering::Relaxed);
    STARTS.lock().unwrap().insert(
        issued,
        Start {
            config,
            seen: Seen {
                calls: vec![format!("attach({planned})")],
                launch_id: k.launch_id,
                plan_sha256: k.plan_sha256,
                planned_dlls: planned,
                session: issued,
                attached_at: Some(std::time::Instant::now()),
                ..Seen::default()
            },
            issued: None,
        },
    );
    LAST.set(issued);
    // SAFETY: the caller's out-pointer.
    unsafe { session.write(issued) };
    KILN_OK
}

unsafe extern "C" fn witness_wait(
    key: *const KilnEmlStartupKeyV1,
    timeout_ms: u32,
    out: *mut KilnEmlStartupWitnessV1,
    reason: *mut c_char,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    let session = match start_of(key) {
        Ok(s) => s,
        Err(e) => return e,
    };
    // SAFETY: the caller's record, its size first.
    if out.is_null()
        || unsafe { (*out).size } < WITNESS_SIZE
        || required.is_null()
        || (reason.is_null() && capacity != 0)
    {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    note(session, format!("witness_wait({timeout_ms})"));
    let config = config(session);
    let fill = |state: i32, plan_id: [u8; 16], revision: u64| {
        // SAFETY: as above; the caller's size is kept, flags and reserved zero.
        unsafe {
            (*out).state = state as u32;
            (*out).plan_id = plan_id;
            (*out).revision = revision;
            (*out).flags = 0;
            (*out).reserved = 0;
        }
    };
    let ready = |plan_id: [u8; 16], revision: u64| {
        if let Some(start) = STARTS.lock().unwrap().get_mut(&session) {
            start.issued = Some((plan_id, revision));
        }
        fill(KILN_EML_WITNESS_READY, plan_id, revision);
        // SAFETY: the caller's required.
        unsafe { required.write(0) };
        KILN_OK
    };
    match config.witness {
        WitnessAnswer::Ready(revision) => ready(config.plan_id, revision),
        WitnessAnswer::ReadyFor(plan_id, revision) => ready(plan_id, revision),
        WitnessAnswer::Refused(text) => {
            let result = write_text(text.as_bytes(), reason, capacity, required);
            if result == KILN_OK {
                fill(KILN_EML_WITNESS_REFUSED, [0; 16], 0);
            }
            result
        }
        WitnessAnswer::Pending => {
            fill(KILN_EML_WITNESS_PENDING, [0; 16], 0);
            // SAFETY: the caller's required.
            unsafe { required.write(0) };
            KILN_OK
        }
        WitnessAnswer::Fail(result) => result,
    }
}

unsafe extern "C" fn witness_recheck(
    key: *const KilnEmlStartupKeyV1,
    witness: *const KilnEmlStartupWitnessV1,
    reason: *mut c_char,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    let session = match start_of(key) {
        Ok(s) => s,
        Err(e) => return e,
    };
    if witness.is_null() || required.is_null() || (reason.is_null() && capacity != 0) {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    // SAFETY: the caller's record.
    let w = unsafe { *witness };
    let issued = STARTS.lock().unwrap()[&session].issued;
    if w.size < WITNESS_SIZE
        || w.flags != 0
        || w.reserved != 0
        || w.state != KILN_EML_WITNESS_READY as u32
        || issued != Some((w.plan_id, w.revision))
    {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    note(session, "witness_recheck".into());
    match config(session).recheck {
        RecheckAnswer::Unchanged => {
            // SAFETY: the caller's required.
            unsafe { required.write(0) };
            KILN_OK
        }
        RecheckAnswer::Changed(text) => write_text(text.as_bytes(), reason, capacity, required),
        RecheckAnswer::Fail(result) => result,
    }
}

unsafe extern "C" fn native_terminal(key: *const KilnEmlStartupKeyV1, json: KilnStr) -> KilnResult {
    let session = match start_of(key) {
        Ok(s) => s,
        Err(e) => return e,
    };
    if json.ptr.is_null() && json.len != 0 {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    // SAFETY: counted UTF-8 for the call.
    let bytes = unsafe { std::slice::from_raw_parts(json.ptr.cast::<u8>(), json.len) };
    let text = String::from_utf8_lossy(bytes).into_owned();
    let mut starts = STARTS.lock().unwrap();
    let start = starts.get_mut(&session).unwrap();
    start.seen.calls.push("native_terminal".into());
    if start.seen.terminal.is_some() {
        return KILN_ERR_UNAVAILABLE;
    }
    let planned = start.seen.planned_dlls as usize;
    start.seen.terminal = Some(text.clone());
    start.seen.pushed_at = Some(std::time::Instant::now());
    start.seen.pushed_on = std::thread::current().name().map(str::to_owned);
    let valid = serde_json::from_str::<Value>(&text)
        .ok()
        .and_then(|v| v["dlls"].as_array().map(Vec::len))
        == Some(planned);
    if !valid {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    start.config.terminal
}

unsafe extern "C" fn outcome(
    key: *const KilnEmlStartupKeyV1,
    identity: *mut KilnEmlArchiveIdentityV1,
    json: *mut c_char,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    let session = match start_of(key) {
        Ok(s) => s,
        Err(e) => return e,
    };
    // SAFETY: the caller's record, its size first.
    if identity.is_null()
        || unsafe { (*identity).size } < IDENTITY_SIZE
        || required.is_null()
        || (json.is_null() && capacity != 0)
    {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    note(session, format!("outcome({capacity})"));
    let (config, seen) = {
        let starts = STARTS.lock().unwrap();
        (
            starts[&session].config.clone(),
            starts[&session].seen.clone(),
        )
    };
    let OutcomeAnswer::Record {
        state,
        dialect,
        rows,
        edit,
    } = config.outcome
    else {
        return KILN_ERR_UNAVAILABLE;
    };
    let grant = matches!(state, "committed" | "cached" | "unchanged" | "restored");
    let mut served = if grant {
        granted_identity(seen.launch_id, config.plan_id, seen.plan_sha256)
    } else {
        // SAFETY: integers and byte arrays.
        let mut zero: KilnEmlArchiveIdentityV1 = unsafe { std::mem::zeroed() };
        zero.size = IDENTITY_SIZE;
        zero
    };
    let mut doc = json!({
        "format": 1,
        "launchId": format!("{:016x}", seen.launch_id),
        "planId": uuid(&config.plan_id),
        "planSha256": hex(&seen.plan_sha256),
        "dialect": dialect,
        "state": state,
        "grant": grant,
        "receiptSha256": Value::Null,
        "archiveSha256": if grant { json!(hex(&served.archive_sha256)) } else { Value::Null },
        "baseOrigin": Value::Null,
        "native": {"received": true, "coherent": true},
        "attempt": if dialect == "none" { "none" } else { "first" },
        "error": Value::Null,
        "retryError": Value::Null,
        "packages": rows,
        "warnings": [],
    });
    if let Some(edit) = edit {
        edit(&mut doc, &mut served);
    }
    let bytes = serde_json::to_vec(&doc).unwrap();
    let result = write_text(&bytes, json, capacity, required);
    if result == KILN_OK {
        // SAFETY: the caller's sized record; its size is kept.
        unsafe {
            served.size = (*identity).size;
            identity.write(served);
        }
        served.size = IDENTITY_SIZE;
        // SAFETY: 160 bytes of plain fields.
        let raw = unsafe { std::mem::transmute::<KilnEmlArchiveIdentityV1, [u8; 160]>(served) };
        STARTS
            .lock()
            .unwrap()
            .get_mut(&session)
            .unwrap()
            .seen
            .served = Some(raw);
    }
    result
}

/// kiln-eml's startup table (v1, 56 bytes; published_identity, kiln.boot's member that kiln-sfi never calls, left null),
/// as a value tests can change one member of.
pub const TABLE_V1: KilnEmlStartupApiV1 = KilnEmlStartupApiV1 {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnEmlStartupApiV1>() as u32,
        version: KILN_EML_STARTUP_VERSION,
    },
    attach: Some(attach),
    witness_wait: Some(witness_wait),
    witness_recheck: Some(witness_recheck),
    native_terminal: Some(native_terminal),
    outcome: Some(outcome),
    published_identity: None,
};
/// The table [`getter`] serves.
pub static TABLE: KilnEmlStartupApiV1 = TABLE_V1;

/// kiln-eml's early export: the table for `min_version` <= 1, NULL above.
pub unsafe extern "C" fn getter(min_version: u32) -> *const KilnEmlStartupApiV1 {
    if min_version > KILN_EML_STARTUP_VERSION {
        return std::ptr::null();
    }
    &raw const TABLE
}
