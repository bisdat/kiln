// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.archive v1's attribute group: attribute_catalog, attribute_resolve and attribute_evaluate, over the archive a
//! generation was authorized with (`view::authorize_with_archive`), reported as `eml.archive.attributes` with its three
//! member rows.
//!
//! The catalog is ShroudForge's (`crate::sf_startup::attributes::Catalog`, the same code the pregame VM answers
//! `runtime.ecs.get_attributes` with): built from the generation's REFERENCE catalog only (`AppState::attribute_catalog`
//! reads the reference index, never a mod's staged records), with a copy of the generation's registry. It is built
//! once per generation, on the first attribute call, and kept with the generation: its result (a failure too) is what
//! every later call of the group answers, as ShroudForge's session catalog keeps its first answer. No lock is held while
//! it builds; when two first calls race, the first result stored is the one every call answers with. Building it reads
//! the archive (IO on the caller's thread); nothing here reads or writes engine memory or enters Lua.
//!
//! Replies (schema/sfi/eml-archive/attribute-*.schema.json, schema_version 1):
//! - attribute_catalog: the pin, every entry in attribute-ID order and the reflected component layouts, each as the
//!   neutral tagged tree of ShroudForge's own JSON (`tagged`): an entry's `descriptor` is ShroudForge's whole descriptor
//!   with its key order; `definition_guid` is the definition GUID of the entry's one associated component (game-order
//!   bytes), null without exactly one; `scalar_type` mirrors the descriptor's;
//! - attribute_resolve: one entry for a selector (attribute-selector.schema.json: an original name or an attribute ID);
//! - attribute_evaluate: ShroudForge's `recalculate` of one complete root over caller-owned bits: the output bits and
//!   the ordered trace of every program run.
//!
//! The JSON replies are materialized once (the catalog's with the catalog, each entry's with it), so a size probe and
//! the copy answer the same bytes. An evaluation is computed per call (pure, over owned data).
//!
//! Failures carry ShroudForge's own text behind an error token (`errors`), never a text invented from the code:
//! - the catalog could not be built: UNAVAILABLE with ShroudForge's reason (e.g. "no attribute definitions in this KFC
//!   container", and every read or decode failure);
//! - resolve: "attribute ID not found" and "attribute not found: <name>" are NOT_FOUND, "Ambiguous attribute name. Use
//!   its ID" is UNAVAILABLE (an ambiguous selector); a selector outside its schema is INVALID_ARGUMENT without a token;
//! - evaluate: "incomplete attribute root" is NOT_FOUND, a count that is not the root's length INVALID_ARGUMENT
//!   ("attribute root value count mismatch"), an engine operation enum that differs from the verified model or a
//!   scalar the programs do not support UNAVAILABLE, and a program that fails on these bits (stack underflow,
//!   non-finite or out-of-bounds values) INVALID_ARGUMENT, each with ShroudForge's text.
use std::{
    collections::BTreeMap,
    ffi::c_char,
    rc::Rc,
    sync::{Arc, Mutex, MutexGuard},
};

use kfc::guid::Guid;
use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_NOT_FOUND, KILN_ERR_UNAVAILABLE, KILN_OK, KilnOwner,
    KilnResult, KilnStr,
    services::kiln_eml_archive::{KilnArchiveErrorV1, KilnEmlArchiveView},
};
use serde_json::{Map, Value, json};

use super::{
    read::{ArchiveBacking, cause, checked, contained, failed, span, target},
    view::Generation,
};
use crate::sf_startup::attributes::{Catalog, program::Scalar, uint};

/// A generation's attribute catalog, as built once (module docs).
pub(crate) enum Materialized {
    Built(Built),
    /// ShroudForge's reason the catalog could not be built.
    Failed(Arc<[u8]>),
}

pub(crate) struct Built {
    /// The catalog (behind a lock: its lookup index is built lazily, on one thread at a time).
    catalog: Mutex<Catalog>,
    /// attribute_catalog's reply.
    reply: Arc<[u8]>,
    /// Each entry's `attribute_entry` JSON, by attribute ID.
    entries: BTreeMap<u32, String>,
}

impl Built {
    fn catalog(&self) -> MutexGuard<'_, Catalog> {
        self.catalog.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

/// The generation's catalog, built from its reference catalog the first time (module docs).
fn materialized(generation: &Generation, backing: &ArchiveBacking) -> Arc<Materialized> {
    if let Some(built) = generation.attributes.get() {
        return built.clone();
    }
    let built = Arc::new(build(generation, backing));
    generation.attributes.get_or_init(|| built).clone()
}

fn build(generation: &Generation, backing: &ArchiveBacking) -> Materialized {
    let registry = generation
        .registry
        .as_ref()
        .expect("a generation with an archive keeps its registry");
    // the catalog decodes through the codec's session types (Rc): a copy of the generation's registry
    let types = Rc::new((**registry).clone());
    let catalog = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        Catalog::load(&types, backing.reference(), |id| backing.reference_bytes(id))
    })) {
        Ok(Ok(catalog)) => catalog,
        Ok(Err(reason)) => return Materialized::Failed(cause(reason)),
        Err(_) => return Materialized::Failed(cause("the attribute catalog could not be built".into())),
    };
    let entries: BTreeMap<u32, String> = catalog
        .entries
        .iter()
        .map(|(id, descriptor)| (*id, entry_json(*id, descriptor)))
        .collect();
    let layouts: Vec<String> = catalog.layouts.iter().map(tagged_text).collect();
    let reply = format!(
        "{{\"schema_version\":1,\"pin\":{},\"entries\":[{}],\"layouts\":[{}]}}",
        generation.pin_json,
        entries.values().cloned().collect::<Vec<_>>().join(","),
        layouts.join(",")
    );
    Materialized::Built(Built {
        catalog: Mutex::new(catalog),
        reply: Arc::from(reply.into_bytes()),
        entries,
    })
}

/// A JSON value as the neutral tagged tree (common.schema.json `value`): an unsigned integer is `u64`, a negative one
/// `i64` (two's-complement bits), any other number `f64` (raw bits), a string `string` (its bytes in hex), an array
/// `array` and an object a `map` with `string` keys in the object's order.
pub(crate) fn tagged(value: &Value) -> Value {
    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }
    let string = |text: &str| json!({"tag":"string","value":format!("string:{}", hex(text.as_bytes()))});
    match value {
        Value::Null => json!({"tag":"nil"}),
        Value::Bool(value) => json!({"tag":"bool","value":value}),
        Value::Number(number) => {
            if let Some(value) = number.as_u64() {
                json!({"tag":"u64","value":format!("u64:{value:016x}")})
            } else if let Some(value) = number.as_i64() {
                json!({"tag":"i64","value":format!("i64:{:016x}", value as u64)})
            } else {
                let bits = number.as_f64().unwrap_or(f64::NAN).to_bits();
                json!({"tag":"f64","value":format!("f64:{bits:016x}")})
            }
        }
        Value::String(text) => string(text),
        Value::Array(items) => json!({"tag":"array","items":items.iter().map(tagged).collect::<Vec<_>>()}),
        Value::Object(map) => {
            let entries: Vec<Value> = map
                .iter()
                .map(|(key, value)| json!({"key":string(key),"value":tagged(value)}))
                .collect();
            json!({"tag":"map","entries":entries})
        }
    }
}

fn tagged_text(value: &Value) -> String {
    tagged(value).to_string()
}

/// The definition GUID (game-order bytes) of a descriptor's one associated component, if it has exactly one with one.
fn definition_guid(descriptor: &Value) -> Value {
    let guids: Vec<&str> = descriptor["components"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|component| component["definition_guid"].as_str())
        .collect();
    match guids.as_slice() {
        [guid] if descriptor["components"].as_array().is_some_and(|c| c.len() == 1) => Guid::parse(guid)
            .map(|guid| {
                let hex: String = guid.data().iter().map(|b| format!("{b:02x}")).collect();
                json!(format!("guid:{hex}"))
            })
            .unwrap_or(Value::Null),
        _ => Value::Null,
    }
}

/// One `attribute_entry`: the identity fields as indexes, and the whole descriptor.
fn entry_json(id: u32, descriptor: &Value) -> String {
    let mut entry = Map::new();
    entry.insert("id".into(), json!(id));
    entry.insert("name".into(), descriptor["name"].clone());
    entry.insert("root_hash".into(), descriptor["root_hash"].clone());
    entry.insert("index".into(), descriptor["index"].clone());
    entry.insert("definition_guid".into(), definition_guid(descriptor));
    entry.insert("scalar_type".into(), descriptor["scalar_type"].clone());
    entry.insert("descriptor".into(), tagged(descriptor));
    Value::Object(entry).to_string()
}

/// A selector (attribute-selector.schema.json): exactly `schema_version` 1 and one of `name` (a string) or `id` (u32).
enum Selector {
    Name(String),
    Id(u32),
}

fn parse_selector(bytes: &[u8]) -> Option<Selector> {
    let Value::Object(map) = serde_json::from_slice::<Value>(bytes).ok()? else {
        return None;
    };
    if map.len() != 2 || map.get("schema_version")?.as_u64() != Some(1) {
        return None;
    }
    if let Some(name) = map.get("name") {
        return Some(Selector::Name(name.as_str()?.to_owned()));
    }
    let id = map.get("id")?.as_u64()?;
    Some(Selector::Id(u32::try_from(id).ok()?))
}

/// Copies a materialized reply: required always, the bytes when they fit.
unsafe fn reply(json: *mut c_char, capacity: usize, required: *mut usize, bytes: &[u8]) -> KilnResult {
    unsafe { required.write_unaligned(bytes.len()) };
    if capacity < bytes.len() {
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), json.cast::<u8>(), bytes.len()) };
    KILN_OK
}

pub(super) unsafe extern "C" fn attribute_catalog(
    owner: KilnOwner,
    registry: KilnEmlArchiveView,
    json: *mut c_char,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let record = match checked(
        error,
        &[],
        &[span(json, capacity), span(required, std::mem::size_of::<usize>())],
    ) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if owner == 0 || required.is_null() || (json.is_null() && capacity != 0) {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let (generation, backing) = match target(owner, registry) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        match &*materialized(&generation, &backing) {
            Materialized::Failed(text) => failed(&record, KILN_ERR_UNAVAILABLE, owner, registry, text.clone()),
            Materialized::Built(built) => {
                let rc = unsafe { reply(json, capacity, required, &built.reply) };
                record.finish(rc, 0, 0)
            }
        }
    })
}

pub(super) unsafe extern "C" fn attribute_resolve(
    owner: KilnOwner,
    registry: KilnEmlArchiveView,
    selector_json: KilnStr,
    json: *mut c_char,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let record = match checked(
        error,
        &[span(selector_json.ptr, selector_json.len)],
        &[span(json, capacity), span(required, std::mem::size_of::<usize>())],
    ) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if owner == 0
            || required.is_null()
            || (json.is_null() && capacity != 0)
            || selector_json.ptr.is_null()
            || selector_json.len > isize::MAX as usize
        {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let text = unsafe { std::slice::from_raw_parts(selector_json.ptr.cast::<u8>(), selector_json.len) };
        let Some(selector) = parse_selector(text) else {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        };
        let (generation, backing) = match target(owner, registry) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        let materialized = materialized(&generation, &backing);
        let built = match &*materialized {
            Materialized::Failed(text) => {
                return failed(&record, KILN_ERR_UNAVAILABLE, owner, registry, text.clone());
            }
            Materialized::Built(built) => built,
        };
        // SF `Catalog::get` (an ID) and `by_name` (an original name), with their texts
        let id = match selector {
            Selector::Id(id) if built.entries.contains_key(&id) => id,
            Selector::Id(_) => {
                return failed(&record, KILN_ERR_NOT_FOUND, owner, registry, cause("attribute ID not found".into()));
            }
            Selector::Name(name) => {
                let found = built.catalog().by_name(&name).map(|descriptor| uint(&descriptor["hash"]));
                match found {
                    Ok(Ok(id)) => id,
                    Ok(Err(reason)) => return failed(&record, KILN_ERR_UNAVAILABLE, owner, registry, cause(reason)),
                    Err(reason) => {
                        let rc = if reason.starts_with("Ambiguous") { KILN_ERR_UNAVAILABLE } else { KILN_ERR_NOT_FOUND };
                        return failed(&record, rc, owner, registry, cause(reason));
                    }
                }
            }
        };
        let Some(entry) = built.entries.get(&id) else {
            return failed(&record, KILN_ERR_NOT_FOUND, owner, registry, cause("attribute ID not found".into()));
        };
        let bytes = format!("{{\"schema_version\":1,\"pin\":{},\"entry\":{entry}}}", generation.pin_json);
        let rc = unsafe { reply(json, capacity, required, bytes.as_bytes()) };
        record.finish(rc, 0, 0)
    })
}

/// SF `recalculate`'s refusals before it runs a program, with the result each answers here (module docs).
fn evaluate_checks(catalog: &Catalog, root: u32, count: usize) -> Result<(), (KilnResult, String)> {
    let entries = catalog.root_entries(root).map_err(|e| (KILN_ERR_NOT_FOUND, e))?;
    let first = entries
        .first()
        .ok_or((KILN_ERR_NOT_FOUND, "incomplete attribute root".to_owned()))?;
    if first["calculation_encoding_verified"] != true {
        return Err((
            KILN_ERR_UNAVAILABLE,
            "engine AttributeOps enum differs from the verified model".into(),
        ));
    }
    if entries.len() != count {
        return Err((KILN_ERR_INVALID_ARGUMENT, "attribute root value count mismatch".into()));
    }
    let scalar = first["calculation_scalar_type"]
        .as_str()
        .ok_or((KILN_ERR_UNAVAILABLE, "unknown scalar".to_owned()))?;
    Scalar::parse(scalar).map_err(|e| (KILN_ERR_UNAVAILABLE, e))?;
    Ok(())
}

pub(super) unsafe extern "C" fn attribute_evaluate(
    owner: KilnOwner,
    registry: KilnEmlArchiveView,
    root_hash: u32,
    input_bits: *const u32,
    count: u32,
    output_bits: *mut u32,
    trace_json: *mut c_char,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let words = count as usize * std::mem::size_of::<u32>();
    let record = match checked(
        error,
        &[span(input_bits, words)],
        &[
            span(output_bits, words),
            span(trace_json, capacity),
            span(required, std::mem::size_of::<usize>()),
        ],
    ) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if owner == 0
            || required.is_null()
            || (trace_json.is_null() && capacity != 0)
            || (count != 0 && (input_bits.is_null() || output_bits.is_null()))
        {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let input: Vec<u32> = if count == 0 {
            Vec::new()
        } else {
            (0..count as usize)
                .map(|i| unsafe { input_bits.add(i).read_unaligned() })
                .collect()
        };
        let (generation, backing) = match target(owner, registry) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        let materialized = materialized(&generation, &backing);
        let built = match &*materialized {
            Materialized::Failed(text) => {
                return failed(&record, KILN_ERR_UNAVAILABLE, owner, registry, text.clone());
            }
            Materialized::Built(built) => built,
        };
        let outcome = {
            let catalog = built.catalog();
            evaluate_checks(&catalog, root_hash, input.len())
                .and_then(|()| catalog.recalculate(root_hash, &input).map_err(|e| (KILN_ERR_INVALID_ARGUMENT, e)))
        };
        let (output, trace) = match outcome {
            Ok(done) => done,
            Err((rc, reason)) => return failed(&record, rc, owner, registry, cause(reason)),
        };
        let trace: Vec<Value> = trace.iter().map(tagged).collect();
        let bytes = format!(
            "{{\"schema_version\":1,\"pin\":{},\"root_hash\":{root_hash},\"input_bits\":{},\"output_bits\":{},\"trace\":{}}}",
            generation.pin_json,
            json!(input),
            json!(output),
            Value::Array(trace),
        );
        unsafe { required.write_unaligned(bytes.len()) };
        if capacity < bytes.len() {
            // nothing else is written: output_bits stay untouched
            return record.finish(KILN_ERR_BUFFER_TOO_SMALL, 0, 0);
        }
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), trace_json.cast::<u8>(), bytes.len()) };
        for (i, bits) in output.iter().enumerate() {
            unsafe { output_bits.add(i).write_unaligned(*bits) };
        }
        record.finish(KILN_OK, 0, 0)
    })
}
