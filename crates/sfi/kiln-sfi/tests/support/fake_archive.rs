// SPDX-License-Identifier: MIT
//! A fake kiln.eml.archive v1 (`native/core/abi/services/kiln_eml_archive.h`) and the kiln.caps
//! `find` it is checked through, built from kiln-abi's generated types.
//!
//! - Grants are per owner ([`grant`]): `acquire` issues a fresh nonzero view only for an
//!   expected identity equal to the grant in every pin (else UNAVAILABLE, the header's
//!   unauthorized pins); `identity` copies the grant (or a [`Grant::lie`] about it).
//! - `registry_metadata` follows the header's sizing rules: `required` is mandatory and set to the
//!   complete length; capacity below it is BUFFER_TOO_SMALL with nothing written; the error
//!   record is checked first and always initialized. A [`Grant::fail_at`] page fails with a fresh
//!   token whose text `error_copy` copies (sized the same way) until `error_release`.
//! - Tokens: release is idempotent; every copy and release is counted per token.
//! - kiln.caps rows are the provider's (metadata and errors groups with their member rows);
//!   [`set_row`] withholds one on the calling thread. Other services are the wrapped lookup's.
//! - kiln.eml.values v1 (`native/core/abi/services/kiln_eml_values.h`) as far as a VM's host drives it:
//!   the arena's lifecycle over the views this fake issued (see "kiln.eml.values" below).
#![allow(dead_code)]
use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_eml_archive::*;
use kiln_abi::services::kiln_eml_values::*;
use kiln_abi::*;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, LazyLock, Mutex, OnceLock};

/// A page's JSON bytes for (first, limit).
pub type Pages = Arc<dyn Fn(u64, u32) -> Vec<u8> + Send + Sync>;

/// What one owner was granted, and how the fake misbehaves for it.
#[derive(Clone)]
pub struct Grant {
    pub identity: KilnEmlArchiveIdentityV1,
    pub pages: Pages,
    /// `identity` answers this instead of the grant.
    pub lie: Option<KilnEmlArchiveIdentityV1>,
    /// The page at this `first` (limit > 0) fails with this result and text.
    pub fail_at: Option<(u64, KilnResult, Vec<u8>)>,
    /// The read after a sizing call needs this many more bytes.
    pub grow: usize,
    /// A sizing call (capacity 0) answers BUFFER_TOO_SMALL with this `required`, whatever
    /// the page's length (a provider claiming an oversized or empty page).
    pub sizing: Option<usize>,
}
impl Grant {
    pub fn new(identity: KilnEmlArchiveIdentityV1, pages: Pages) -> Self {
        Self {
            identity,
            pages,
            lie: None,
            fail_at: None,
            grow: 0,
            sizing: None,
        }
    }
}

/// What the fake saw for one owner.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Seen {
    /// Every call, in order: "acquire", "identity", "registry_metadata(first,limit,capacity,record)",
    /// "release(view)", "error_copy(token,capacity)", "error_release(token)".
    pub calls: Vec<String>,
    pub views: Vec<KilnEmlArchiveView>,
    /// Views released, each time.
    pub released: Vec<KilnEmlArchiveView>,
    /// Tokens issued, and per token how often it was released.
    pub tokens: Vec<KilnHandle>,
    pub token_releases: HashMap<KilnHandle, u32>,
}

struct State {
    grants: HashMap<KilnOwner, Grant>,
    seen: HashMap<KilnOwner, Seen>,
    views: HashMap<KilnEmlArchiveView, KilnOwner>,
    tokens: HashMap<KilnHandle, (KilnOwner, Vec<u8>)>,
}
static STATE: LazyLock<Mutex<State>> = LazyLock::new(|| {
    Mutex::new(State {
        grants: HashMap::new(),
        seen: HashMap::new(),
        views: HashMap::new(),
        tokens: HashMap::new(),
    })
});
static NEXT: AtomicU64 = AtomicU64::new(0x5f00);

pub fn grant(owner: KilnOwner, grant: Grant) {
    STATE.lock().unwrap().grants.insert(owner, grant);
}
pub fn seen(owner: KilnOwner) -> Seen {
    STATE
        .lock()
        .unwrap()
        .seen
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}
fn note(owner: KilnOwner, call: String) {
    STATE
        .lock()
        .unwrap()
        .seen
        .entry(owner)
        .or_default()
        .calls
        .push(call);
}

/// The identity a fixture page's `pin` carries (01234567-89ab-4cde-8fab-0123456789ab,
/// fedcba98-7654-4321-8fed-cba987654321, generation 7, epoch 2, digests 11.., 22.., 33..,
/// registry 3), PUBLISHED.
pub fn fixture_identity() -> KilnEmlArchiveIdentityV1 {
    KilnEmlArchiveIdentityV1 {
        size: std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32,
        phase: KILN_EML_ARCHIVE_PUBLISHED as u32,
        launch_id: [
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0x4c, 0xde, 0x8f, 0xab, 0x01, 0x23, 0x45, 0x67,
            0x89, 0xab,
        ],
        plan_id: [
            0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x43, 0x21, 0x8f, 0xed, 0xcb, 0xa9, 0x87, 0x65,
            0x43, 0x21,
        ],
        generation: 7,
        reload_epoch: 2,
        plan_sha256: [0x11; 32],
        archive_sha256: [0x22; 32],
        registry_id: 3,
        registry_sha256: [0x33; 32],
    }
}

fn same(a: &KilnEmlArchiveIdentityV1, b: &KilnEmlArchiveIdentityV1) -> bool {
    a.phase == b.phase
        && a.launch_id == b.launch_id
        && a.plan_id == b.plan_id
        && a.generation == b.generation
        && a.reload_epoch == b.reload_epoch
        && a.plan_sha256 == b.plan_sha256
        && a.archive_sha256 == b.archive_sha256
        && a.registry_id == b.registry_id
        && a.registry_sha256 == b.registry_sha256
}

const IDENTITY_SIZE: u32 = std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;
const RECORD_SIZE: u32 = std::mem::size_of::<KilnArchiveErrorV1>() as u32;

unsafe extern "C" fn acquire(
    owner: KilnOwner,
    expected: *const KilnEmlArchiveIdentityV1,
    out: *mut KilnEmlArchiveView,
) -> KilnResult {
    note(owner, "acquire".into());
    if expected.is_null() || out.is_null() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    // SAFETY: the caller's record.
    let expected = unsafe { *expected };
    if expected.size < IDENTITY_SIZE {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let mut state = STATE.lock().unwrap();
    let Some(grant) = state.grants.get(&owner) else {
        return KILN_ERR_UNAVAILABLE;
    };
    if !same(&grant.identity, &expected) {
        return KILN_ERR_UNAVAILABLE;
    }
    let view = NEXT.fetch_add(1, Ordering::Relaxed);
    state.views.insert(view, owner);
    state.seen.entry(owner).or_default().views.push(view);
    // SAFETY: the caller's out-pointer.
    unsafe { out.write(view) };
    KILN_OK
}

fn owner_of(view: KilnEmlArchiveView) -> Option<KilnOwner> {
    STATE.lock().unwrap().views.get(&view).copied()
}

unsafe extern "C" fn identity(
    view: KilnEmlArchiveView,
    out: *mut KilnEmlArchiveIdentityV1,
) -> KilnResult {
    let Some(owner) = owner_of(view) else {
        return KILN_ERR_REVOKED;
    };
    note(owner, "identity".into());
    // SAFETY: the caller's record, its size first.
    if out.is_null() || unsafe { (*out).size } < IDENTITY_SIZE {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let grant = STATE.lock().unwrap().grants[&owner].clone();
    let mut answer = grant.lie.unwrap_or(grant.identity);
    // SAFETY: as above; the caller's size is kept.
    answer.size = unsafe { (*out).size };
    unsafe { out.write(answer) };
    KILN_OK
}

unsafe extern "C" fn release(view: KilnEmlArchiveView) -> KilnResult {
    let mut state = STATE.lock().unwrap();
    let Some(owner) = state.views.get(&view).copied() else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    let seen = state.seen.entry(owner).or_default();
    seen.calls.push(format!("release({view})"));
    seen.released.push(view);
    drop(state);
    view_released(view);
    KILN_OK
}

fn view_open(view: KilnEmlArchiveView) -> Result<KilnOwner, KilnResult> {
    let state = STATE.lock().unwrap();
    let owner = *state.views.get(&view).ok_or(KILN_ERR_INVALID_ARGUMENT)?;
    if state.seen[&owner].released.contains(&view) {
        return Err(KILN_ERR_REVOKED);
    }
    Ok(owner)
}

#[allow(clippy::too_many_arguments)]
unsafe extern "C" fn registry_metadata(
    view: KilnEmlArchiveView,
    first: u64,
    limit: u32,
    json: *mut c_char,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    // The record is checked first and always initialized.
    if !error.is_null() {
        // SAFETY: the caller's record.
        let record = unsafe { &mut *error };
        if record.size < RECORD_SIZE || record.flags != 0 || record.reserved != 0 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        record.result = KILN_OK;
        record.token = 0;
        record.message_bytes = 0;
    }
    let reply = move |result: KilnResult| {
        if !error.is_null() {
            // SAFETY: as above.
            unsafe { (*error).result = result };
        }
        result
    };
    if required.is_null() || (json.is_null() && capacity != 0) {
        return reply(KILN_ERR_INVALID_ARGUMENT);
    }
    let Some(owner) = owner_of(view) else {
        return reply(KILN_ERR_REVOKED);
    };
    note(
        owner,
        format!(
            "registry_metadata({first},{limit},{capacity},{})",
            if error.is_null() { "null" } else { "record" }
        ),
    );
    let (grant, released) = {
        let state = STATE.lock().unwrap();
        let released = state.seen[&owner].released.contains(&view);
        (state.grants[&owner].clone(), released)
    };
    if released {
        return reply(KILN_ERR_REVOKED);
    }
    if let Some((at, result, text)) = &grant.fail_at
        && *at == first
        && limit > 0
    {
        if !error.is_null() {
            let token = NEXT.fetch_add(1, Ordering::Relaxed);
            let mut state = STATE.lock().unwrap();
            state.tokens.insert(token, (owner, text.clone()));
            state.seen.entry(owner).or_default().tokens.push(token);
            // SAFETY: as above.
            unsafe {
                (*error).token = token;
                (*error).message_bytes = text.len() as u64;
            }
        }
        return reply(*result);
    }
    if capacity == 0
        && let Some(claim) = grant.sizing
    {
        // SAFETY: the caller's required.
        unsafe { required.write(claim) };
        return reply(KILN_ERR_BUFFER_TOO_SMALL);
    }
    let page = (grant.pages)(first, limit);
    let len = page.len() + if capacity == 0 { 0 } else { grant.grow };
    // SAFETY: the caller's required.
    unsafe { required.write(len) };
    if capacity < len {
        return reply(KILN_ERR_BUFFER_TOO_SMALL);
    }
    // SAFETY: `json` holds `capacity` >= len bytes.
    unsafe { std::ptr::copy_nonoverlapping(page.as_ptr(), json.cast::<u8>(), page.len()) };
    reply(KILN_OK)
}

unsafe extern "C" fn error_copy(
    owner: KilnOwner,
    token: KilnHandle,
    out: *mut c_void,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    note(owner, format!("error_copy({token},{capacity})"));
    if required.is_null() || (out.is_null() && capacity != 0) {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let state = STATE.lock().unwrap();
    let Some((issued_to, text)) = state.tokens.get(&token) else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    if *issued_to != owner {
        return KILN_ERR_REVOKED;
    }
    if state.seen[&owner].token_releases.contains_key(&token) {
        return KILN_ERR_REVOKED;
    }
    // SAFETY: the caller's required.
    unsafe { required.write(text.len()) };
    if capacity < text.len() {
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    // SAFETY: `out` holds `capacity` bytes.
    unsafe { std::ptr::copy_nonoverlapping(text.as_ptr(), out.cast::<u8>(), text.len()) };
    KILN_OK
}

unsafe extern "C" fn error_release(owner: KilnOwner, token: KilnHandle) -> KilnResult {
    note(owner, format!("error_release({token})"));
    let mut state = STATE.lock().unwrap();
    match state.tokens.get(&token) {
        None => KILN_ERR_INVALID_ARGUMENT,
        Some((issued_to, _)) if *issued_to != owner => KILN_ERR_REVOKED,
        Some(_) => {
            *state
                .seen
                .entry(owner)
                .or_default()
                .token_releases
                .entry(token)
                .or_default() += 1;
            KILN_OK
        }
    }
}

pub static TABLE: KilnEmlArchiveApiV1 = KilnEmlArchiveApiV1 {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnEmlArchiveApiV1>() as u32,
        version: KILN_EML_ARCHIVE_VERSION,
    },
    acquire: Some(acquire),
    identity: Some(identity),
    resource_json: None,
    release: Some(release),
    registry_metadata: Some(registry_metadata),
    find_resource: None,
    list_resources: None,
    read_resource: None,
    content_read_range: None,
    list_contents: None,
    value_decode: None,
    value_encode: None,
    loaded_field: None,
    attribute_catalog: None,
    attribute_resolve: None,
    attribute_evaluate: None,
    error_copy: Some(error_copy),
    error_release: Some(error_release),
};

// ---- kiln.caps find ----

/// The rows the provider reports available (archive_service.rs GROUPS).
pub const ROWS: [&str; 8] = [
    "eml.archive.metadata",
    "eml.archive.metadata.acquire",
    "eml.archive.metadata.identity",
    "eml.archive.metadata.release",
    "eml.archive.metadata.registry_metadata",
    "eml.archive.errors",
    "eml.archive.errors.error_copy",
    "eml.archive.errors.error_release",
];

/// How a row reads on this thread.
#[derive(Clone, Copy, Debug)]
pub enum Row {
    /// Nobody reported it: find answers NOT_FOUND.
    Unreported,
    /// Reported with this state and text.
    State(i32, &'static str),
}
/// How the archive table is served on this thread.
#[derive(Clone, Copy, Debug)]
pub enum Table {
    Missing,
    /// A copy with this edit.
    Edited(fn(&mut KilnEmlArchiveApiV1)),
}
thread_local! {
    static ROW_SHAPES: RefCell<HashMap<&'static str, Row>> = RefCell::new(HashMap::new());
    static TABLE_SHAPE: RefCell<Option<Table>> = const { RefCell::new(None) };
    static ROW_TEXT: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}
/// `row` reads as `shape` on this thread (`None`: the provider's AVAILABLE row).
pub fn set_row(row: &'static str, shape: Option<Row>) {
    ROW_SHAPES.with_borrow_mut(|rows| match shape {
        Some(shape) => rows.insert(row, shape),
        None => rows.remove(row),
    });
}
pub fn set_table(shape: Option<Table>) {
    TABLE_SHAPE.with_borrow_mut(|t| *t = shape);
}

fn text_of(text: KilnStr) -> String {
    // SAFETY: the caller's counted name.
    let bytes = unsafe { std::slice::from_raw_parts(text.ptr.cast::<u8>(), text.len) };
    String::from_utf8_lossy(bytes).into_owned()
}

unsafe extern "C" fn find(name: KilnStr, out: *mut KilnCapRow) -> KilnResult {
    let name = text_of(name);
    if out.is_null() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let Some(known) = ROWS.iter().find(|r| **r == name) else {
        return KILN_ERR_NOT_FOUND;
    };
    let (state, text) = match ROW_SHAPES.with_borrow(|rows| rows.get(known).copied()) {
        Some(Row::Unreported) => return KILN_ERR_NOT_FOUND,
        Some(Row::State(state, text)) => (state, text),
        None => (KILN_CAP_AVAILABLE, ""),
    };
    // The row's strings live in this thread's storage until its next find (kiln_caps.h).
    let text = ROW_TEXT.with_borrow_mut(|stored| {
        *stored = text.as_bytes().to_vec();
        KilnStr {
            ptr: stored.as_ptr().cast(),
            len: stored.len(),
        }
    });
    // SAFETY: the caller's sized row.
    unsafe {
        (*out).state = state as u32;
        (*out).reason = 0;
        (*out).flags = 0;
        (*out).name = KilnStr {
            ptr: known.as_ptr().cast(),
            len: known.len(),
        };
        (*out).text = text;
        (*out).source = KilnStr {
            ptr: "kiln-eml".as_ptr().cast(),
            len: 8,
        };
    }
    KILN_OK
}

// ---- kiln.eml.values: the arena lifecycle a VM's host drives ----
//
// `arena_create` resolves its view as kiln-eml does: one this fake issued (else INVALID_ARGUMENT) and not released
// (else REVOKED); the host owner is nonzero and the phase STARTUP or RUNTIME (else INVALID_ARGUMENT); the reader is
// admitted as kiln-eml's adapter admits it (size, version 1, access/copy/retain/release present). `arena_close` runs on
// any thread; `arena_collect` only on the arena's creator thread (else WRONG_THREAD); both answer the pending count
// (0, or 1 for good with [`values_never_drain`]). A never-issued control is NOT_FOUND. The value members are never
// reached by a host and answer UNAVAILABLE, except `value_open` with [`values_open_reads`]. Every step is recorded per
// owner as "arena<N>:<step>" (N counts that owner's arenas from 1; [`values_events`]), every `arena_close` call
// included (a second close shows), and handed to the observer ([`observe_values`]); releasing the view an arena was
// made over records "arena<N>:view-released", and "arena<N>:VIOLATION" when the arena had not drained.

/// One arena as the fake saw it.
#[derive(Clone, Debug)]
pub struct ArenaSeen {
    pub n: u32,
    pub owner: KilnOwner,
    pub view: KilnEmlArchiveView,
    pub phase: u32,
    pub control: KilnHandle,
    pub closed: bool,
    pub drained: bool,
    pub collects: u32,
    pub view_released: bool,
}

struct ArenaState {
    seen: ArenaSeen,
    thread: std::thread::ThreadId,
    user: usize,
    wake: Option<unsafe extern "C" fn(*mut c_void, KilnHandle)>,
    access: Option<AccessFn>,
}

type AccessFn = unsafe extern "C" fn(
    *mut c_void,
    *const KilnEmlLuaAccessV1,
    *mut KilnEmlLuaValueV1,
) -> KilnResult;

#[derive(Default)]
struct ValuesKnobs {
    create_answer: Option<KilnResult>,
    never_drain: bool,
    no_control: bool,
    open_reads: Option<&'static str>,
}

static ARENAS: LazyLock<Mutex<HashMap<KilnHandle, ArenaState>>> = LazyLock::new(Default::default);
static VALUES_KNOBS: LazyLock<Mutex<HashMap<KilnOwner, ValuesKnobs>>> =
    LazyLock::new(Default::default);
static VALUES_EVENTS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> =
    LazyLock::new(Default::default);
/// Where the steps also go (a test's ordered record).
type Observer = fn(KilnOwner, String);
static OBSERVER: Mutex<Option<Observer>> = Mutex::new(None);

/// `owner`'s next arena_create answers `answer` (None: as the contract says).
pub fn values_create_answer(owner: KilnOwner, answer: Option<KilnResult>) {
    VALUES_KNOBS
        .lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .create_answer = answer;
}
/// `owner`'s arenas never reach pending 0 (a provider that keeps a host token for good).
pub fn values_never_drain(owner: KilnOwner, never: bool) {
    VALUES_KNOBS
        .lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .never_drain = never;
}
/// `owner`'s next arenas are admitted (the reader kept) with OK but no control token: a provider fault the host can
/// never close or drain.
pub fn values_create_no_control(owner: KilnOwner, no_control: bool) {
    VALUES_KNOBS
        .lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .no_control = no_control;
}
/// `owner`'s `value_open` reads field `key` of its Lua object once through the arena's reader (GET_FIELD_VALUE,
/// recorded as "arena<N>:read:error" or "arena<N>:read:value") and, as Source's struct get turns a failed field read
/// into MissingField, swallows what it got: the answer is a HANDLED nil. `None`: UNAVAILABLE again.
pub fn values_open_reads(owner: KilnOwner, key: Option<&'static str>) {
    VALUES_KNOBS
        .lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .open_reads = key;
}
/// Every step is also handed to `observer` (a test's ordered record).
pub fn observe_values(observer: Option<Observer>) {
    *OBSERVER.lock().unwrap() = observer;
}
/// `owner`'s steps, in order.
pub fn values_events(owner: KilnOwner) -> Vec<String> {
    VALUES_EVENTS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}
/// `owner`'s arenas, in creation order.
pub fn arenas(owner: KilnOwner) -> Vec<ArenaSeen> {
    let mut seen: Vec<ArenaSeen> = ARENAS
        .lock()
        .unwrap()
        .values()
        .filter(|a| a.seen.owner == owner)
        .map(|a| a.seen.clone())
        .collect();
    seen.sort_by_key(|a| a.n);
    seen
}
/// The provider's `wake(user, control)` for `owner`'s arena `n`, from this thread (as a close off the worker does);
/// false when that arena is not open.
pub fn wake(owner: KilnOwner, n: u32) -> bool {
    let target = ARENAS
        .lock()
        .unwrap()
        .values()
        .find(|a| a.seen.owner == owner && a.seen.n == n && !a.seen.closed)
        .map(|a| (a.user, a.wake, a.seen.control));
    match target {
        Some((user, Some(wake), control)) => {
            // SAFETY: the reader the arena admitted; its user lives until the arena drained (it is open)
            unsafe { wake(user as *mut c_void, control) };
            true
        }
        _ => false,
    }
}

fn values_event(owner: KilnOwner, text: String) {
    VALUES_EVENTS
        .lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .push(text.clone());
    let observer = *OBSERVER.lock().unwrap();
    if let Some(observer) = observer {
        observer(owner, text);
    }
}

fn view_released(view: KilnEmlArchiveView) {
    let mut events = Vec::new();
    for arena in ARENAS.lock().unwrap().values_mut() {
        if arena.seen.view == view && !arena.seen.view_released {
            arena.seen.view_released = true;
            events.push((
                arena.seen.owner,
                format!("arena{}:view-released", arena.seen.n),
            ));
            if !arena.seen.drained {
                events.push((arena.seen.owner, format!("arena{}:VIOLATION", arena.seen.n)));
            }
        }
    }
    for (owner, text) in events {
        values_event(owner, text);
    }
}

unsafe extern "C" fn arena_create(
    owner: KilnOwner,
    view: KilnEmlArchiveView,
    phase: u32,
    reader: *const KilnEmlLuaReaderV1,
    out: *mut KilnHandle,
) -> KilnResult {
    if out.is_null() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    // SAFETY: the caller's output, cleared first
    unsafe { out.write(0) };
    if owner == 0 || (phase != KILN_VALUE_STARTUP as u32 && phase != KILN_VALUE_RUNTIME as u32) {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    if reader.is_null()
        // SAFETY: the size field first, then the covered prefix
        || (unsafe { (*reader).size } as usize) < std::mem::size_of::<KilnEmlLuaReaderV1>()
    {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    // SAFETY: as above
    let table = unsafe { reader.read() };
    if table.version != 1
        || table.access.is_none()
        || table.copy.is_none()
        || table.retain.is_none()
        || table.release.is_none()
    {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    if let Err(code) = view_open(view) {
        return code;
    }
    let (answer, no_control) = VALUES_KNOBS
        .lock()
        .unwrap()
        .get(&owner)
        .map_or((None, false), |k| (k.create_answer, k.no_control));
    let mut arenas = ARENAS.lock().unwrap();
    let n = arenas.values().filter(|a| a.seen.owner == owner).count() as u32 + 1;
    if let Some(answer) = answer {
        drop(arenas);
        values_event(owner, format!("arena{n}:create-refused"));
        return answer;
    }
    let control = NEXT.fetch_add(1, Ordering::Relaxed);
    arenas.insert(
        control,
        ArenaState {
            seen: ArenaSeen {
                n,
                owner,
                view,
                phase,
                control,
                closed: false,
                drained: false,
                collects: 0,
                view_released: false,
            },
            thread: std::thread::current().id(),
            user: table.user as usize,
            wake: table.wake,
            access: table.access,
        },
    );
    drop(arenas);
    if no_control {
        // admitted (the reader is kept), but the host gets no control to close it with
        values_event(owner, format!("arena{n}:create-no-control"));
        return KILN_OK;
    }
    values_event(owner, format!("arena{n}:create"));
    // SAFETY: the caller's output
    unsafe { out.write(control) };
    KILN_OK
}

fn never_drains(owner: KilnOwner) -> bool {
    VALUES_KNOBS
        .lock()
        .unwrap()
        .get(&owner)
        .is_some_and(|k| k.never_drain)
}

/// One step of arena `control` (`close` or `collect`): the pending count written to `pending`, the step recorded.
fn arena_step(control: KilnHandle, step: &str, pending: *mut u32) -> Result<(), KilnResult> {
    let mut arenas = ARENAS.lock().unwrap();
    let arena = arenas.get_mut(&control).ok_or(KILN_ERR_NOT_FOUND)?;
    let owner = arena.seen.owner;
    if step == "collect" && arena.thread != std::thread::current().id() {
        return Err(KILN_ERR_WRONG_THREAD);
    }
    if step == "close" {
        arena.seen.closed = true;
    } else {
        arena.seen.collects += 1;
    }
    let n = arena.seen.n;
    let left = if arena.seen.closed && !never_drains(owner) {
        0
    } else {
        u32::from(arena.seen.closed)
    };
    let drained_now = arena.seen.closed && left == 0 && !arena.seen.drained;
    if drained_now {
        arena.seen.drained = true;
    }
    drop(arenas);
    if !pending.is_null() {
        // SAFETY: the caller's output
        unsafe { pending.write(left) };
    }
    values_event(owner, format!("arena{n}:{step}"));
    if drained_now {
        values_event(owner, format!("arena{n}:drained"));
    }
    Ok(())
}

unsafe extern "C" fn arena_close(
    _: KilnOwner,
    control: KilnHandle,
    pending: *mut u32,
) -> KilnResult {
    arena_step(control, "close", pending)
        .err()
        .unwrap_or(KILN_OK)
}
unsafe extern "C" fn arena_collect(
    _: KilnOwner,
    control: KilnHandle,
    pending: *mut u32,
) -> KilnResult {
    arena_step(control, "collect", pending)
        .err()
        .unwrap_or(KILN_OK)
}

// The value members: no host reaches them here, but `value_open` with [`values_open_reads`].
unsafe extern "C" fn value_open(
    owner: KilnOwner,
    control: KilnHandle,
    _: KilnEmlArchiveView,
    open: *const KilnEmlValueOpenV1,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    let key = VALUES_KNOBS
        .lock()
        .unwrap()
        .get(&owner)
        .and_then(|k| k.open_reads);
    let arena = ARENAS
        .lock()
        .unwrap()
        .get(&control)
        .map(|a| (a.seen.n, a.user, a.access));
    let (Some(key), Some((n, user, Some(access)))) = (key, arena) else {
        return KILN_ERR_UNAVAILABLE;
    };
    if open.is_null() || result.is_null() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    // SAFETY: the caller's open record, read for this call
    let object = unsafe { (*open).lua_object };
    let request = KilnEmlLuaAccessV1 {
        size: std::mem::size_of::<KilnEmlLuaAccessV1>() as u32,
        operation: KILN_LUA_GET_FIELD_VALUE as u32,
        object,
        index: 0,
        key: KilnStr {
            ptr: key.as_ptr().cast(),
            len: key.len(),
        },
        arena: 0,
        node_id: 0,
    };
    // SAFETY: an all-zero descriptor is valid (integers only); the reader writes it whole
    let mut reply: KilnEmlLuaValueV1 = unsafe { std::mem::zeroed() };
    reply.size = std::mem::size_of::<KilnEmlLuaValueV1>() as u32;
    // SAFETY: the reader this arena admitted, on the worker that runs this open; no lock is held across it (the
    // field read runs Lua)
    let code = unsafe { access(user as *mut c_void, &raw const request, &raw mut reply) };
    let outcome = match (code, reply.error_object) {
        (KILN_OK, 0) => "value".to_owned(),
        (KILN_OK, _) => "error".to_owned(),
        (code, _) => format!("refused{code}"),
    };
    values_event(owner, format!("arena{n}:read:{outcome}"));
    // SAFETY: the caller's result record
    unsafe {
        result.write(KilnEmlValueResultV1 {
            size: std::mem::size_of::<KilnEmlValueResultV1>() as u32,
            disposition: KILN_VALUE_HANDLED as u32,
            flags: 0,
            arity: 1,
            item: 0,
            payload: 0,
            error: 0,
            receiver_revision: 0,
        })
    };
    KILN_OK
}
unsafe extern "C" fn value_info(
    _: KilnOwner,
    _: KilnHandle,
    _: *mut KilnEmlValueInfoV1,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn value_read(
    _: KilnOwner,
    _: KilnHandle,
    _: u32,
    _: *mut KilnEmlValueResultV1,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn value_selector(
    _: KilnOwner,
    _: KilnHandle,
    _: *const KilnEmlValueSelectorV1,
    _: *mut KilnEmlValueResultV1,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn value_set(
    _: KilnOwner,
    _: KilnHandle,
    _: *const KilnEmlValueSelectorV1,
    _: u64,
    _: *mut KilnEmlValueResultV1,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn value_unary(
    _: KilnOwner,
    _: KilnHandle,
    _: *mut KilnEmlValueResultV1,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn value_export(
    _: KilnOwner,
    _: KilnHandle,
    _: KilnStr,
    _: *mut KilnEmlValueResultV1,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn value_close(_: KilnOwner, _: KilnHandle) -> KilnResult {
    KILN_ERR_NOT_FOUND
}
unsafe extern "C" fn value_result_copy(
    _: KilnOwner,
    _: KilnHandle,
    _: *mut c_void,
    _: usize,
    _: *mut usize,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn value_with_u64(
    _: KilnOwner,
    _: KilnHandle,
    _: u64,
    _: *mut KilnEmlValueResultV1,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn value_insert(
    _: KilnOwner,
    _: KilnHandle,
    _: u64,
    _: u64,
    _: *mut KilnEmlValueResultV1,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}

pub static VALUES_TABLE: KilnEmlValuesApiV1 = KilnEmlValuesApiV1 {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnEmlValuesApiV1>() as u32,
        version: KILN_EML_VALUES_VERSION,
    },
    arena_create: Some(arena_create),
    arena_close: Some(arena_close),
    arena_collect: Some(arena_collect),
    value_open: Some(value_open),
    value_info: Some(value_info),
    value_read: Some(value_read),
    value_child: Some(value_selector),
    value_set: Some(value_set),
    value_clone: Some(value_unary),
    value_export: Some(value_export),
    value_close: Some(value_close),
    value_result_copy: Some(value_result_copy),
    value_array_push: Some(value_with_u64),
    value_array_insert: Some(value_insert),
    value_array_pop: Some(value_unary),
    value_array_remove: Some(value_with_u64),
    value_array_clear: Some(value_unary),
};

// ---- the lookup ----

type Lookup = unsafe extern "C" fn(KilnStr, u32) -> *const c_void;
static INNER: AtomicUsize = AtomicUsize::new(0);
static CAPS: OnceLock<KilnCapsApi> = OnceLock::new();

/// `api`'s service lookup now also serves kiln.eml.archive, kiln.eml.values and a kiln.caps with
/// `find` (the wrapped lookup's kiln.caps members, when it has one, are kept).
pub fn wrap(api: &mut KilnApi) {
    let inner = api.service.expect("a service lookup to wrap");
    INNER.store(inner as usize, Ordering::SeqCst);
    api.service = Some(lookup);
}

fn inner() -> Lookup {
    let address = INNER.load(Ordering::SeqCst);
    assert_ne!(address, 0, "wrap() first");
    // SAFETY: stored from a Lookup in wrap().
    unsafe { std::mem::transmute::<usize, Lookup>(address) }
}

unsafe extern "C" fn lookup(name: KilnStr, version: u32) -> *const c_void {
    match text_of(name).as_str() {
        "kiln.eml.values" => (&raw const VALUES_TABLE).cast(),
        "kiln.eml.archive" => match TABLE_SHAPE.with_borrow(|t| *t) {
            None => (&raw const TABLE).cast(),
            Some(Table::Missing) => std::ptr::null(),
            Some(Table::Edited(edit)) => {
                let mut copy = TABLE;
                edit(&mut copy);
                (Box::leak(Box::new(copy)) as *const KilnEmlArchiveApiV1).cast()
            }
        },
        "kiln.caps" => {
            let caps = CAPS.get_or_init(|| {
                // SAFETY: the wrapped lookup, with a static name.
                let base = unsafe { inner()(name, version) }.cast::<KilnCapsApi>();
                let mut caps = if base.is_null() {
                    KilnCapsApi {
                        header: KilnServiceHeader {
                            size: std::mem::size_of::<KilnCapsApi>() as u32,
                            version: KILN_CAPS_VERSION,
                        },
                        count: None,
                        at: None,
                        find: None,
                        report: None,
                        subscribe: None,
                        unsubscribe: None,
                        generation: None,
                    }
                } else {
                    // SAFETY: a full kiln.caps table of the test support.
                    unsafe { *base }
                };
                caps.find = Some(find);
                caps
            });
            (caps as *const KilnCapsApi).cast()
        }
        // SAFETY: the caller's arguments, passed on.
        _ => unsafe { inner()(name, version) },
    }
}
