// SPDX-License-Identifier: MIT
//! A fake kiln.eml.archive attribute group (`native/core/abi/services/kiln_eml_archive.h`) over the fake services'
//! lookup, for the attribute family's host tests. Per thread (every call is direct, on the test's thread):
//! - `acquire` issues a view for the identity [`set`] gave (else UNAVAILABLE), `identity` copies it, `release` notes it;
//! - `attribute_catalog` follows the header's sizing rules (a probe gives the complete length, a short buffer writes
//!   nothing) and answers the reply [`State::catalog`] holds, or its failure with a fresh error token;
//! - `attribute_evaluate` answers [`State::evaluate`] (output bits and trace, or a failure with a token), leaving the
//!   outputs untouched on a short trace buffer;
//! - `error_copy` / `error_release` serve the tokens.
//!
//! Every call is noted in [`State::calls`]. [`lookup`] serves this table as `kiln.eml.archive` (or a copy without the
//! attribute slots, [`State::no_slots`]) and passes every other name to the fake services' lookup.
#![allow(dead_code)]
use kiln_abi::services::kiln_eml_archive::*;
use kiln_abi::*;
use serde_json::{Value as Json, json};
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};
use std::rc::Rc;

/// What `attribute_evaluate` answers for (root, input bits).
pub type Evaluate = Rc<dyn Fn(u32, &[u32]) -> Result<(Vec<u32>, Vec<Json>), (KilnResult, String)>>;

#[derive(Default)]
pub struct State {
    pub identity: Option<KilnEmlArchiveIdentityV1>,
    /// attribute_catalog's reply, or its failure.
    pub catalog: Option<Result<Vec<u8>, (KilnResult, String)>>,
    pub evaluate: Option<Evaluate>,
    pub calls: Vec<String>,
    pub released: Vec<KilnEmlArchiveView>,
    pub no_slots: bool,
    /// kiln.engine's `engine.attribute_model` row: AVAILABLE with this text, UNAVAILABLE with this reason, or none.
    pub model: Option<Result<String, String>>,
    tokens: HashMap<KilnHandle, Vec<u8>>,
    next: u64,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State { next: 0x7a00, ..State::default() });
}

/// Changes (or reads) this thread's state.
pub fn with<R>(f: impl FnOnce(&mut State) -> R) -> R {
    STATE.with_borrow_mut(f)
}

/// The calls of `member` so far.
pub fn calls(member: &str) -> usize {
    with(|s| s.calls.iter().filter(|c| c.as_str() == member).count())
}

/// The tagged tree (common.schema.json `value`) of a JSON value, as kiln-eml's provider writes it.
pub fn tagged(value: &Json) -> Json {
    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }
    let string =
        |text: &str| json!({"tag":"string","value":format!("string:{}", hex(text.as_bytes()))});
    match value {
        Json::Null => json!({"tag":"nil"}),
        Json::Bool(b) => json!({"tag":"bool","value":b}),
        Json::Number(n) => {
            if let Some(v) = n.as_u64() {
                json!({"tag":"u64","value":format!("u64:{v:016x}")})
            } else if let Some(v) = n.as_i64() {
                json!({"tag":"i64","value":format!("i64:{:016x}", v as u64)})
            } else {
                json!({"tag":"f64","value":format!("f64:{:016x}", n.as_f64().unwrap().to_bits())})
            }
        }
        Json::String(text) => string(text),
        Json::Array(items) => {
            json!({"tag":"array","items":items.iter().map(tagged).collect::<Vec<_>>()})
        }
        Json::Object(map) => {
            json!({"tag":"map","entries":map.iter().map(|(k, v)| json!({"key":string(k),"value":tagged(v)})).collect::<Vec<_>>()})
        }
    }
}

fn u64t(value: u64) -> String {
    format!("u64:{value:016x}")
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn uuid(b: &[u8; 16]) -> String {
    let h = hex(b);
    format!(
        "{}-{}-{}-{}-{}",
        &h[0..8],
        &h[8..12],
        &h[12..16],
        &h[16..20],
        &h[20..32]
    )
}

/// The `pin` object of `identity`.
pub fn pin(identity: &KilnEmlArchiveIdentityV1) -> Json {
    json!({"launch_id":uuid(&identity.launch_id),"plan_id":uuid(&identity.plan_id),
        "generation":u64t(identity.generation),"reload_epoch":u64t(identity.reload_epoch),
        "plan_sha256":hex(&identity.plan_sha256),"archive_sha256":hex(&identity.archive_sha256),
        "registry_id":u64t(identity.registry_id),"registry_sha256":hex(&identity.registry_sha256)})
}

/// attribute_catalog's reply over `descriptors` (by ID) and `layouts`, for `identity`.
pub fn catalog_reply(
    identity: &KilnEmlArchiveIdentityV1,
    descriptors: &[Json],
    layouts: &[Json],
) -> Vec<u8> {
    let mut sorted: Vec<&Json> = descriptors.iter().collect();
    sorted.sort_by_key(|d| d["hash"].as_u64());
    let entries: Vec<Json> = sorted
        .iter()
        .map(|d| {
            json!({"id":d["hash"],"name":d["name"],"root_hash":d["root_hash"],"index":d["index"],
                "definition_guid":null,"scalar_type":d["scalar_type"],"descriptor":tagged(d)})
        })
        .collect();
    serde_json::to_vec(
        &json!({"schema_version":1,"pin":pin(identity),"entries":entries,
        "layouts":layouts.iter().map(tagged).collect::<Vec<_>>()}),
    )
    .unwrap()
}

fn note(call: &str) {
    with(|s| s.calls.push(call.to_owned()));
}

fn issue(text: &str) -> KilnHandle {
    with(|s| {
        s.next += 1;
        s.tokens.insert(s.next, text.as_bytes().to_vec());
        s.next
    })
}

/// Fills the caller's error record (when there is one) and answers `result`.
unsafe fn finish(
    error: *mut KilnArchiveErrorV1,
    result: KilnResult,
    token: KilnHandle,
    bytes: usize,
) -> KilnResult {
    if !error.is_null() {
        // SAFETY: the caller's v1 record.
        unsafe {
            (*error).result = result;
            (*error).token = token;
            (*error).message_bytes = bytes as u64;
            (*error).flags = 0;
            (*error).reserved = 0;
        }
    }
    result
}

unsafe fn fail(error: *mut KilnArchiveErrorV1, result: KilnResult, text: &str) -> KilnResult {
    let token = if error.is_null() { 0 } else { issue(text) };
    unsafe { finish(error, result, token, text.len()) }
}

unsafe extern "C" fn acquire(
    _owner: KilnOwner,
    expected: *const KilnEmlArchiveIdentityV1,
    out: *mut KilnEmlArchiveView,
) -> KilnResult {
    note("acquire");
    let granted = with(|s| s.identity);
    // SAFETY: the caller's record.
    let want = unsafe { *expected };
    match granted {
        Some(identity)
            if identity.generation == want.generation
                && identity.registry_id == want.registry_id =>
        {
            let view = with(|s| {
                s.next += 1;
                s.next
            });
            // SAFETY: the caller's out pointer.
            unsafe { *out = view };
            KILN_OK
        }
        _ => KILN_ERR_UNAVAILABLE,
    }
}

unsafe extern "C" fn identity(
    _view: KilnEmlArchiveView,
    out: *mut KilnEmlArchiveIdentityV1,
) -> KilnResult {
    note("identity");
    let Some(mut identity) = with(|s| s.identity) else {
        return KILN_ERR_REVOKED;
    };
    // SAFETY: the caller's sized record.
    identity.size = unsafe { (*out).size };
    unsafe { *out = identity };
    KILN_OK
}

unsafe extern "C" fn release(view: KilnEmlArchiveView) -> KilnResult {
    note("release");
    with(|s| s.released.push(view));
    KILN_OK
}

unsafe extern "C" fn attribute_catalog(
    _owner: KilnOwner,
    _view: KilnEmlArchiveView,
    json: *mut c_char,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    note("attribute_catalog");
    match with(|s| s.catalog.clone()) {
        None => unsafe { finish(error, KILN_ERR_UNAVAILABLE, 0, 0) },
        Some(Err((result, text))) => unsafe { fail(error, result, &text) },
        Some(Ok(bytes)) => {
            // SAFETY: the caller's out pointer.
            unsafe { *required = bytes.len() };
            if capacity < bytes.len() {
                return unsafe { finish(error, KILN_ERR_BUFFER_TOO_SMALL, 0, 0) };
            }
            // SAFETY: `json` holds `capacity` bytes.
            unsafe {
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), json.cast::<u8>(), bytes.len())
            };
            unsafe { finish(error, KILN_OK, 0, 0) }
        }
    }
}

unsafe extern "C" fn attribute_resolve(
    _: KilnOwner,
    _: KilnEmlArchiveView,
    _: KilnStr,
    _: *mut c_char,
    _: usize,
    _: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    note("attribute_resolve");
    unsafe { finish(error, KILN_ERR_UNAVAILABLE, 0, 0) }
}

#[allow(clippy::too_many_arguments)]
unsafe extern "C" fn attribute_evaluate(
    _owner: KilnOwner,
    _view: KilnEmlArchiveView,
    root: u32,
    input: *const u32,
    count: u32,
    output: *mut u32,
    trace_json: *mut c_char,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    note("attribute_evaluate");
    // SAFETY: `count` words.
    let bits: Vec<u32> = (0..count as usize)
        .map(|i| unsafe { *input.add(i) })
        .collect();
    let Some(evaluate) = with(|s| s.evaluate.clone()) else {
        return unsafe { finish(error, KILN_ERR_UNAVAILABLE, 0, 0) };
    };
    match evaluate(root, &bits) {
        Err((result, text)) => unsafe { fail(error, result, &text) },
        Ok((out, trace)) => {
            let reply = serde_json::to_vec(
                &json!({"schema_version":1,"root_hash":root,"input_bits":bits,
                "output_bits":out,"trace":trace.iter().map(tagged).collect::<Vec<_>>()}),
            )
            .unwrap();
            // SAFETY: the caller's out pointer.
            unsafe { *required = reply.len() };
            if capacity < reply.len() {
                return unsafe { finish(error, KILN_ERR_BUFFER_TOO_SMALL, 0, 0) };
            }
            // SAFETY: the caller's buffers.
            unsafe {
                std::ptr::copy_nonoverlapping(reply.as_ptr(), trace_json.cast::<u8>(), reply.len());
                for (i, word) in out.iter().enumerate() {
                    *output.add(i) = *word;
                }
            }
            unsafe { finish(error, KILN_OK, 0, 0) }
        }
    }
}

unsafe extern "C" fn error_copy(
    _owner: KilnOwner,
    token: KilnHandle,
    out: *mut c_void,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    note("error_copy");
    let Some(text) = with(|s| s.tokens.get(&token).cloned()) else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    // SAFETY: the caller's out pointer.
    unsafe { *required = text.len() };
    if capacity < text.len() {
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    // SAFETY: `out` holds `capacity` bytes.
    unsafe { std::ptr::copy_nonoverlapping(text.as_ptr(), out.cast::<u8>(), text.len()) };
    KILN_OK
}

unsafe extern "C" fn error_release(_owner: KilnOwner, token: KilnHandle) -> KilnResult {
    note("error_release");
    with(|s| s.tokens.remove(&token));
    KILN_OK
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
    registry_metadata: None,
    find_resource: None,
    list_resources: None,
    read_resource: None,
    content_read_range: None,
    list_contents: None,
    value_decode: None,
    value_encode: None,
    loaded_field: None,
    attribute_catalog: Some(attribute_catalog),
    attribute_resolve: Some(attribute_resolve),
    attribute_evaluate: Some(attribute_evaluate),
    error_copy: Some(error_copy),
    error_release: Some(error_release),
};

static NO_SLOTS: KilnEmlArchiveApiV1 = KilnEmlArchiveApiV1 {
    attribute_catalog: None,
    attribute_resolve: None,
    attribute_evaluate: None,
    ..TABLE
};

/// The rows the attribute family checks: the metadata group (the view), the attribute group and the errors channel.
pub const ROWS: [&str; 12] = [
    "eml.archive.metadata",
    "eml.archive.metadata.acquire",
    "eml.archive.metadata.identity",
    "eml.archive.metadata.release",
    "eml.archive.metadata.registry_metadata",
    "eml.archive.attributes",
    "eml.archive.attributes.attribute_catalog",
    "eml.archive.attributes.attribute_resolve",
    "eml.archive.attributes.attribute_evaluate",
    "eml.archive.errors",
    "eml.archive.errors.error_copy",
    "eml.archive.errors.error_release",
];

/// `KilnApi.service`: this table as kiln.eml.archive, every other name the fake services'.
pub unsafe extern "C" fn lookup(name: KilnStr, version: u32) -> *const c_void {
    // SAFETY: the caller's counted name.
    let text = unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) };
    if text == b"kiln.eml.archive" {
        return if with(|s| s.no_slots) {
            (&raw const NO_SLOTS).cast()
        } else {
            (&raw const TABLE).cast()
        };
    }
    if text == b"kiln.caps" {
        return CAPS.get_or_init(|| {
            let mut caps = super::ecs::fake_services::CAPS_TABLE;
            caps.find = Some(caps_find);
            SendCaps(caps)
        }) as *const SendCaps as *const c_void;
    }
    // SAFETY: passed on.
    unsafe { super::ecs::fake_services::lookup(name, version) }
}

/// kiln.caps with the fake services' rows, and kiln.engine's `engine.attribute_model` row with its text from
/// [`State::model`] (None: nobody reported it).
#[repr(transparent)]
struct SendCaps(kiln_abi::services::kiln_caps::KilnCapsApi);
// SAFETY: plain data and function pointers.
unsafe impl Sync for SendCaps {}
static CAPS: std::sync::OnceLock<SendCaps> = std::sync::OnceLock::new();
thread_local! {
    static MODEL_TEXT: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

unsafe extern "C" fn caps_find(
    name: KilnStr,
    out: *mut kiln_abi::services::kiln_caps::KilnCapRow,
) -> KilnResult {
    use kiln_abi::services::kiln_caps::*;
    // SAFETY: the caller's counted name.
    let text = unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) };
    if text != b"engine.attribute_model" {
        let find = super::ecs::fake_services::CAPS_TABLE.find.unwrap();
        // SAFETY: passed on.
        return unsafe { find(name, out) };
    }
    let Some(model) = with(|s| s.model.clone()) else {
        return KILN_ERR_NOT_FOUND;
    };
    let (state, text) = match model {
        Ok(model) => (KILN_CAP_AVAILABLE, model),
        Err(why) => (KILN_CAP_UNAVAILABLE, why),
    };
    let text = MODEL_TEXT.with_borrow_mut(|stored| {
        *stored = text.into_bytes();
        KilnStr {
            ptr: stored.as_ptr().cast(),
            len: stored.len(),
        }
    });
    // SAFETY: the caller's sized row.
    unsafe {
        (*out).state = state as u32;
        (*out).reason = if state == KILN_CAP_AVAILABLE {
            KILN_CAP_REASON_NONE
        } else {
            KILN_CAP_REASON_SYMBOL_MISSING
        } as u32;
        (*out).flags = 0;
        (*out).name = KilnStr {
            ptr: b"engine.attribute_model".as_ptr().cast(),
            len: 22,
        };
        (*out).text = text;
        (*out).source = KilnStr {
            ptr: b"kiln.engine".as_ptr().cast(),
            len: 11,
        };
    }
    KILN_OK
}
