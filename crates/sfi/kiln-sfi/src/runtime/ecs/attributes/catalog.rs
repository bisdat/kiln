// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/reflection/build_attribute_catalog.rs (the catalog's name and root
// indexes, `by_name`, `root_entries`, `read_header`, `bind_storage`), lua/ingame/ecs/attributes/mod.rs:7-20
// (`Catalog::get`), session/mod.rs:123-136 (one catalog per session, its first answer kept).
//! The attribute catalog of the archive generation granted to this start, through kiln.eml.archive's attribute group.
//!
//! [`Attributes`] opens it once per VM, at the first attribute call, on the worker thread:
//! 1. the table (`KILN_EML_ARCHIVE_SERVICE` v1) and its `attribute_catalog` and `attribute_evaluate` slots (extent and
//!    non-null);
//! 2. the kiln.caps rows `eml.archive.attributes` and its three member rows, each AVAILABLE (a held, off or absent row
//!    refuses use), and the precise-error channel when `eml.archive.errors` and its rows are there;
//! 3. a view of the granted generation ([`GrantedView`]: the metadata rows, `acquire`, `identity` equal in every pin);
//! 4. `attribute_catalog`: a sizing call, then the read into exactly that many bytes. Its `pin` must be the view's.
//!    Each entry's descriptor and each layout come back from the neutral tagged tree ([`untag`]) as ShroudForge's own
//!    JSON values.
//!
//! The result, a failure too, is the VM's for good (ShroudForge's `AppState::attribute_catalog` keeps its first answer).
//! A failure the archive reports keeps the provider's own text (ShroudForge's, copied from its error token); a Kiln
//! failure on the way (a missing slot or row, a view refused) is Kiln's text. The view is released when the VM's
//! attributes go.
//!
//! [`Open::recalculate`] is ShroudForge's `Catalog::recalculate` on the archive (`attribute_evaluate`): the output bits
//! and the ordered trace, or the provider's text. Every call is direct (no hop, no wait) and enters no Lua.

use super::uint;
use crate::runtime::services::{Member, ServiceAdapter, ServiceTable};
use crate::runtime::types::GrantedView;
use crate::runtime::types::archive::pin_of;
use crate::service_member;
use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_eml_archive::*;
use kiln_abi::*;
use mlua::Value as LuaValue;
use serde_json::{Map, Value, json};
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::ffi::{c_char, c_void};
use std::rc::Rc;

type CatalogFn = unsafe extern "C" fn(
    KilnOwner,
    KilnEmlArchiveView,
    *mut c_char,
    usize,
    *mut usize,
    *mut KilnArchiveErrorV1,
) -> KilnResult;
type EvaluateFn = unsafe extern "C" fn(
    KilnOwner,
    KilnEmlArchiveView,
    u32,
    *const u32,
    u32,
    *mut u32,
    *mut c_char,
    usize,
    *mut usize,
    *mut KilnArchiveErrorV1,
) -> KilnResult;
type ErrorCopyFn =
    unsafe extern "C" fn(KilnOwner, KilnHandle, *mut c_void, usize, *mut usize) -> KilnResult;
type ErrorReleaseFn = unsafe extern "C" fn(KilnOwner, KilnHandle) -> KilnResult;

/// The attribute group's rows.
pub const ATTRIBUTE_ROWS: [&str; 4] = [
    "eml.archive.attributes",
    "eml.archive.attributes.attribute_catalog",
    "eml.archive.attributes.attribute_resolve",
    "eml.archive.attributes.attribute_evaluate",
];
/// The precise-error channel's rows.
const ERROR_ROWS: [&str; 3] = [
    "eml.archive.errors",
    "eml.archive.errors.error_copy",
    "eml.archive.errors.error_release",
];
/// The largest catalog reply this reader takes.
pub const CATALOG_MAX: usize = 256 << 20;
/// The largest error text copied.
const MESSAGE_MAX: usize = 1 << 20;

/// Kiln's text when the catalog cannot be had (the archive's own failures keep their text instead).
pub fn unavailable(why: impl std::fmt::Display) -> String {
    format!("kiln: the archive's attribute catalog is not available: {why}")
}

/// ShroudForge's attribute catalog, as the archive built it: the descriptors by attribute ID and the reflected
/// component layouts, with ShroudForge's name and root indexes.
#[derive(Default)]
pub struct Catalog {
    pub entries: BTreeMap<u32, Value>,
    pub layouts: Vec<Value>,
    // None keeps a duplicated name ambiguous instead of picking the first ID.
    names: BTreeMap<String, Option<u32>>,
    // None marks a root whose reflected indices are missing or duplicated.
    roots: BTreeMap<u32, Option<Vec<u32>>>,
}

impl Catalog {
    /// The catalog of `entries` (descriptors by ID) and `layouts`, with ShroudForge's indexes.
    pub fn new(entries: BTreeMap<u32, Value>, layouts: Vec<Value>) -> Self {
        let mut names = BTreeMap::new();
        let mut roots = BTreeMap::<u32, Vec<u32>>::new();
        for (&id, entry) in &entries {
            if let Some(name) = entry["name"].as_str() {
                names
                    .entry(name.to_owned())
                    .and_modify(|existing| *existing = None)
                    .or_insert(Some(id));
            }
            if let Ok(root) = uint(&entry["root_hash"]) {
                roots.entry(root).or_default().push(id);
            }
        }
        let roots = roots
            .into_iter()
            .map(|(root, mut ids)| {
                ids.sort_by_key(|id| entries[id]["index"].as_u64());
                let complete = ids
                    .iter()
                    .enumerate()
                    .all(|(index, id)| entries[id]["index"] == index);
                (root, complete.then_some(ids))
            })
            .collect();
        Self {
            entries,
            layouts,
            names,
            roots,
        }
    }

    /// SF `by_name`: a unique original name's descriptor; a duplicated name is refused, never the first match.
    pub fn by_name(&self, name: &str) -> Result<&Value, String> {
        match self.names.get(name) {
            Some(Some(id)) => self
                .entries
                .get(id)
                .ok_or_else(|| "attribute ID not found".into()),
            Some(None) => Err("Ambiguous attribute name. Use its ID".into()),
            None => Err(format!("attribute not found: {name}")),
        }
    }

    /// SF `Catalog::get`: an unsigned attribute ID or an original name.
    pub fn get(&self, selector: &LuaValue) -> Result<&Value, String> {
        match selector {
            LuaValue::Integer(id) => self
                .entries
                .get(&u32::try_from(*id).map_err(|_| "attribute ID outside u32")?)
                .ok_or_else(|| "attribute ID not found".into()),
            LuaValue::String(name) => {
                let name = name.to_str().map_err(|e| e.to_string())?;
                self.by_name(name.as_ref())
            }
            _ => Err("expected original attribute name or unsigned attribute ID".into()),
        }
    }

    /// SF `root_entries`: a root's descriptors in reflected index order; a root with a missing or repeated index is
    /// incomplete.
    pub fn root_entries(&self, root: u32) -> Result<Vec<&Value>, String> {
        let ids = self
            .roots
            .get(&root)
            .and_then(Option::as_ref)
            .ok_or("incomplete attribute root")?;
        ids.iter()
            .map(|id| {
                self.entries
                    .get(id)
                    .ok_or_else(|| "incomplete attribute root".into())
            })
            .collect()
    }

    /// SF `bind_storage`: `descriptor` bound to one component layout: its storage and calculation writability for
    /// that component's scalar type. Kiln adds one case ShroudForge refuses: a root of [`ENGINE_SIGNED_ROOTS`] with
    /// the resource's keen::sint32 over keen::uint32 storage is calculated as keen::sint32, the engine's own
    /// interpreter for it ([`engine_signed_root`]).
    pub fn bind_storage(&self, descriptor: &Value, component: &Value) -> Result<Value, String> {
        let root = uint(&descriptor["root_hash"])?;
        let count = self.root_entries(root)?.len();
        if component["storage_size"].as_u64() != Some(count as u64 * 4) {
            return Err(text::STORAGE_COUNT.into());
        }
        let scalar = descriptor["scalar_type"].as_str();
        let storage = component["storage_scalar_type"].as_str();
        let exact = scalar.is_some() && scalar == storage;
        let integer = matches!(scalar, Some("keen::sint32" | "keen::uint32"))
            && matches!(storage, Some("keen::sint32" | "keen::uint32"));
        let engine_signed = engine_signed_root(root, scalar, storage);
        let calculation = exact
            || (integer && (descriptor["calculation_sign_independent"] == true || engine_signed));
        let mut result = descriptor.clone();
        result["components"] = json!([component]);
        result["storage_writable"] = json!(exact || integer);
        result["storage_reason"] = if exact || integer {
            Value::Null
        } else {
            json!(text::SCALAR_DIFFERS)
        };
        result["write_value_domain"] = json!(if !exact && integer {
            "integer-common-range-0-to-2147483647"
        } else {
            "declared-scalar"
        });
        result["calculation_writable"] = json!(calculation);
        result["calculation_reason"] = if calculation {
            Value::Null
        } else {
            json!("signedness-dependent program with conflicting resource/storage scalar types")
        };
        if calculation {
            result["calculation_scalar_type"] = if engine_signed {
                json!(scalar)
            } else {
                component["storage_scalar_type"].clone()
            };
        }
        Ok(result)
    }
}

/// The roots whose calculation Kiln admits although their resource and storage scalar types differ in signedness and
/// their programs depend on it (ShroudForge refuses them all): BodyHeat (0xC7AEAF1D) alone. The game's attribute
/// resource types the root keen::sint32; its component, keen::ecs::BodyHeat, is a keen::ecs::UInt32Attribute whose
/// storage is keen::uint32; BodyHeat runs Clamp and BodyHeat_Max runs ScaleToNewMax. The engine evaluates this root
/// with its signed interpreter only (client exe 5d9ef2f1, Steam build 26430625; server exe f0d824be): its attribute
/// dispatcher picks the interpreter by the registry's resource type (sint32: the signed one), and the BodyHeat
/// recharge and depletion code and attrAdd recompute the root through the signed recalculation (docs/re/combat.md,
/// "BodyHeat runs"). So the root is calculated as the resource's keen::sint32, which is also what the archive's
/// attribute_evaluate runs; the write domain stays the common range 0..2147483647 for every word before and after
/// (`validate_attribute_bits`), where both readings of the storage agree. Every other conflicting root stays refused.
pub const ENGINE_SIGNED_ROOTS: [u32; 1] = [0xC7AE_AF1D];

/// Whether `root` is one of [`ENGINE_SIGNED_ROOTS`] with exactly the pair proven for it: the resource's keen::sint32
/// over keen::uint32 storage. The reverse pair, a float, or another root is not admitted.
pub fn engine_signed_root(root: u32, scalar: Option<&str>, storage: Option<&str>) -> bool {
    scalar == Some("keen::sint32")
        && storage == Some("keen::uint32")
        && ENGINE_SIGNED_ROOTS.contains(&root)
}

/// ShroudForge's texts that name the archive format (kept out of this file's source lines whole: the licence guard
/// scans MIT sources for the parser crate's name).
pub mod text {
    /// build_attribute_catalog.rs `bind_storage`.
    pub const STORAGE_COUNT: &str = concat!(
        "attribute component storage count differs from ",
        "K",
        "FC root"
    );
    /// build_attribute_catalog.rs `bind_storage` and `from_resources`.
    pub const SCALAR_DIFFERS: &str = concat!("K", "FC scalar type differs from reflected storage");
    /// read_attribute_storage_range.rs:18.
    pub const ROOT_DIFFERS: &str =
        concat!("live attribute root ID differs from ", "K", "FC definition");
    /// read_attribute_storage_range.rs:30.
    pub const DEFINITION_DIFFERS: &str = concat!(
        "live attribute definition differs from ",
        "K",
        "FC definition"
    );
}

/// SF `read_header`: one reflected header field of an attribute component's bytes, as u32.
pub fn read_header(header: &Value, name: &str, bytes: &[u8]) -> Result<u32, String> {
    let field = &header[name];
    let offset = uint(&field["offset"])? as usize;
    let size = uint(&field["size"])? as usize;
    let value = bytes
        .get(
            offset
                ..offset
                    .checked_add(size)
                    .ok_or("attribute header offset overflow")?,
        )
        .ok_or("attribute header outside component")?;
    match size {
        1 => Ok(u32::from(value[0])),
        2 => Ok(u32::from(u16::from_le_bytes([value[0], value[1]]))),
        4 => Ok(u32::from_le_bytes([value[0], value[1], value[2], value[3]])),
        _ => Err("unsupported reflected attribute header width".into()),
    }
}

/// A GUID's game-order bytes from its text (the standard 8-4-4-4-12 form, the first three groups little-endian), as
/// the catalog writes `definition_guid`; None for any other text.
pub fn guid_bytes(text: &str) -> Option<[u8; 16]> {
    let text = text.as_bytes();
    if text.len() < 36 || [8, 13, 18, 23].iter().any(|&i| text[i] != b'-') {
        return None;
    }
    let digit = |i: usize| (text[i] as char).to_digit(16).map(|d| d as u8);
    let byte = |i: usize| Some(digit(i)? << 4 | digit(i + 1)?);
    let run = |start: usize, len: usize| -> Option<Vec<u8>> {
        (0..len).map(|k| byte(start + 2 * k)).collect()
    };
    let (a, b, c, d, e) = (
        run(0, 4)?,
        run(9, 2)?,
        run(14, 2)?,
        run(19, 2)?,
        run(24, 6)?,
    );
    let mut bytes = [0u8; 16];
    bytes[..4].copy_from_slice(&[a[3], a[2], a[1], a[0]]);
    bytes[4..6].copy_from_slice(&[b[1], b[0]]);
    bytes[6..8].copy_from_slice(&[c[1], c[0]]);
    bytes[8..10].copy_from_slice(&d);
    bytes[10..].copy_from_slice(&e);
    Some(bytes)
}

fn unhex(text: &str) -> Option<Vec<u8>> {
    if !text.len().is_multiple_of(2) {
        return None;
    }
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(text.get(i..i + 2)?, 16).ok())
        .collect()
}

/// A neutral tagged tree (schema/sfi/eml-archive/common.schema.json `value`) back as the JSON value it carries: `u64`
/// and `i64` as integers, `f64` and `f32` as numbers, `string` as text (it must be UTF-8), `array` and `map` (string
/// keys, in order) as arrays and objects, `nil` as null. Anything else is refused: the catalog is plain JSON.
pub fn untag(node: &Value) -> Result<Value, String> {
    let bad = || format!("the archive's attribute catalog has a malformed value: {node}");
    let text = |prefix: &str| -> Result<&str, String> {
        node["value"]
            .as_str()
            .and_then(|t| t.strip_prefix(prefix))
            .ok_or_else(bad)
    };
    let bits = |prefix: &str| -> Result<u64, String> {
        u64::from_str_radix(text(prefix)?, 16).map_err(|_| bad())
    };
    Ok(match node["tag"].as_str().ok_or_else(bad)? {
        "nil" => Value::Null,
        "bool" => Value::Bool(node["value"].as_bool().ok_or_else(bad)?),
        "u64" => json!(bits("u64:")?),
        "i64" => json!(bits("i64:")? as i64),
        "f64" => json!(f64::from_bits(bits("f64:")?)),
        "f32" => json!(f64::from(f32::from_bits(
            u32::try_from(bits("f32:")?).map_err(|_| bad())?
        ))),
        "string" => {
            let bytes = unhex(text("string:")?).ok_or_else(bad)?;
            Value::String(String::from_utf8(bytes).map_err(|_| bad())?)
        }
        "array" => Value::Array(
            node["items"]
                .as_array()
                .ok_or_else(bad)?
                .iter()
                .map(untag)
                .collect::<Result<_, _>>()?,
        ),
        "map" => {
            let mut map = Map::new();
            for entry in node["entries"].as_array().ok_or_else(bad)? {
                let Value::String(key) = untag(&entry["key"])? else {
                    return Err(bad());
                };
                map.insert(key, untag(&entry["value"])?);
            }
            Value::Object(map)
        }
        _ => return Err(bad()),
    })
}

struct ErrorChannel {
    copy: Member<ErrorCopyFn>,
    release: Member<ErrorReleaseFn>,
}

/// The opened catalog: the view, the evaluate slot, the error channel and the catalog itself.
pub struct Open {
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
    view: GrantedView,
    evaluate: Member<EvaluateFn>,
    errors: Option<ErrorChannel>,
    pub catalog: Catalog,
}

fn error_record() -> KilnArchiveErrorV1 {
    KilnArchiveErrorV1 {
        size: std::mem::size_of::<KilnArchiveErrorV1>() as u32,
        result: KILN_OK,
        token: 0,
        message_bytes: 0,
        flags: 0,
        reserved: 0,
    }
}

/// kiln.caps `find`: whether `row` is reported AVAILABLE, and why not.
fn require(services: &ServiceAdapter, row: &str) -> Result<(), String> {
    let table = services
        .bind::<KilnCapsApi>(KILN_CAPS_SERVICE, KILN_CAPS_VERSION)
        .map_err(|e| e.to_string())?;
    let find = service_member!(table, KilnCapsApi, find).map_err(|e| e.to_string())?;
    // SAFETY: an all-zero row is valid (integers and empty strings).
    let mut found: KilnCapRow = unsafe { std::mem::zeroed() };
    found.size = std::mem::size_of::<KilnCapRow>() as u32;
    let name = KilnStr {
        ptr: row.as_ptr().cast(),
        len: row.len(),
    };
    // SAFETY: a counted name and a sized caller row (kiln_caps.h).
    let result = unsafe { services.call(find, (name, &raw mut found)) };
    if result == KILN_OK && found.state == KILN_CAP_AVAILABLE as u32 {
        Ok(())
    } else {
        Err(format!("{row} is not available"))
    }
}

impl Open {
    fn record_ptr(&self, record: &mut KilnArchiveErrorV1) -> *mut KilnArchiveErrorV1 {
        if self.errors.is_some() {
            record
        } else {
            std::ptr::null_mut()
        }
    }

    /// The provider's text behind a failed call's token (copied, then released once), else Kiln's text for `result`.
    fn failure(&self, member: &str, result: KilnResult, record: &KilnArchiveErrorV1) -> String {
        let text = match &self.errors {
            Some(channel) if record.token != 0 => {
                let text = copy_message(&self.services, self.owner, channel, record.token);
                // SAFETY: a token this owner's call was issued; released once, here.
                let _ = unsafe {
                    self.services
                        .call(channel.release, (self.owner, record.token))
                };
                text
            }
            _ => None,
        };
        text.unwrap_or_else(|| unavailable(format!("{member} answered {result}")))
    }

    /// SF `Catalog::recalculate` (the archive's `attribute_evaluate`): the root's values after its programs ran, from
    /// its last entry to its first, and the trace of every program run.
    pub fn recalculate(&self, root: u32, values: &[u32]) -> Result<(Vec<u32>, Vec<Value>), String> {
        let count =
            u32::try_from(values.len()).map_err(|_| "attribute root value count mismatch")?;
        let mut output = vec![0u32; values.len()];
        let mut trace: Vec<u8> = Vec::new();
        for _ in 0..2 {
            let mut record = error_record();
            let record_ptr = self.record_ptr(&mut record);
            let mut required = 0usize;
            let data: *mut c_char = if trace.is_empty() {
                std::ptr::null_mut()
            } else {
                trace.as_mut_ptr().cast()
            };
            // SAFETY: the view this VM acquired; `values` and `output` hold `count` words and do not overlap; `data` is
            // NULL with capacity 0 or writable for its length; required and the record are live.
            let result = unsafe {
                self.services.call(
                    self.evaluate,
                    (
                        self.owner,
                        self.view.view(),
                        root,
                        values.as_ptr(),
                        count,
                        output.as_mut_ptr(),
                        data,
                        trace.len(),
                        &raw mut required,
                        record_ptr,
                    ),
                )
            };
            if result == KILN_ERR_BUFFER_TOO_SMALL && trace.is_empty() && required <= CATALOG_MAX {
                trace = vec![0u8; required];
                continue;
            }
            if result != KILN_OK {
                return Err(self.failure("attribute_evaluate", result, &record));
            }
            let reply: Value = serde_json::from_slice(&trace[..required.min(trace.len())])
                .map_err(|_| unavailable("attribute_evaluate's reply is not JSON"))?;
            let steps = reply["trace"]
                .as_array()
                .ok_or_else(|| unavailable("attribute_evaluate's reply has no trace"))?
                .iter()
                .map(untag)
                .collect::<Result<Vec<_>, _>>()?;
            return Ok((output, steps));
        }
        Err(unavailable("attribute_evaluate changed its size"))
    }
}

fn copy_message(
    services: &ServiceAdapter,
    owner: KilnOwner,
    channel: &ErrorChannel,
    token: KilnHandle,
) -> Option<String> {
    let mut required = 0usize;
    // SAFETY: a probe: NULL data with capacity 0 and a live required.
    let result = unsafe {
        services.call(
            channel.copy,
            (owner, token, std::ptr::null_mut(), 0, &raw mut required),
        )
    };
    if result == KILN_OK {
        return (required == 0).then(String::new);
    }
    if result != KILN_ERR_BUFFER_TOO_SMALL || required == 0 || required > MESSAGE_MAX {
        return None;
    }
    let mut text = vec![0u8; required];
    let mut written = 0usize;
    // SAFETY: `text` is writable for `required` bytes.
    let result = unsafe {
        services.call(
            channel.copy,
            (
                owner,
                token,
                text.as_mut_ptr().cast::<c_void>(),
                required,
                &raw mut written,
            ),
        )
    };
    if result != KILN_OK || written > required {
        return None;
    }
    text.truncate(written);
    Some(String::from_utf8_lossy(&text).into_owned())
}

/// One VM's attribute catalog: opened at the first use, kept with its first answer (module docs).
pub struct Attributes {
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
    identity: KilnEmlArchiveIdentityV1,
    open: RefCell<Option<Result<Rc<Open>, String>>>,
}

impl Attributes {
    /// The catalog of the generation `identity` pins, read under `owner` (kiln-sfi's plugin owner).
    pub fn new(
        services: Rc<ServiceAdapter>,
        owner: KilnOwner,
        identity: KilnEmlArchiveIdentityV1,
    ) -> Self {
        Self {
            services,
            owner,
            identity,
            open: RefCell::new(None),
        }
    }

    /// SF `AppState::attribute_catalog`: the catalog, opened the first time, or the reason it is not there.
    pub fn open(&self) -> Result<Rc<Open>, String> {
        if let Some(done) = self.open.borrow().as_ref() {
            return done.clone();
        }
        let done = self.read().map(Rc::new);
        *self.open.borrow_mut() = Some(done.clone());
        done
    }

    fn read(&self) -> Result<Open, String> {
        let services = &self.services;
        let table: ServiceTable<KilnEmlArchiveApiV1> = services
            .bind(KILN_EML_ARCHIVE_SERVICE, KILN_EML_ARCHIVE_VERSION)
            .map_err(unavailable)?;
        let catalog_member: Member<CatalogFn> =
            service_member!(table, KilnEmlArchiveApiV1, attribute_catalog).map_err(unavailable)?;
        let evaluate: Member<EvaluateFn> =
            service_member!(table, KilnEmlArchiveApiV1, attribute_evaluate).map_err(unavailable)?;
        for row in ATTRIBUTE_ROWS {
            require(services, row).map_err(unavailable)?;
        }
        let errors = (|| {
            let copy = service_member!(table, KilnEmlArchiveApiV1, error_copy).ok()?;
            let release = service_member!(table, KilnEmlArchiveApiV1, error_release).ok()?;
            ERROR_ROWS
                .iter()
                .all(|row| require(services, row).is_ok())
                .then_some(ErrorChannel { copy, release })
        })();
        let view =
            GrantedView::acquire(services, self.owner, &self.identity).map_err(unavailable)?;
        let mut open = Open {
            services: services.clone(),
            owner: self.owner,
            view,
            evaluate,
            errors,
            catalog: Catalog::default(),
        };
        let bytes = Self::catalog_bytes(&open, catalog_member)?;
        let reply: Value = serde_json::from_slice(&bytes)
            .map_err(|_| unavailable("attribute_catalog's reply is not JSON"))?;
        let pin = pin_of(&self.identity);
        if reply["schema_version"] != 1
            || reply["pin"]["generation"] != format!("u64:{:016x}", pin.generation)
            || reply["pin"]["registry_id"] != format!("u64:{:016x}", pin.registry_id)
            || reply["pin"]["registry_sha256"] != pin.registry_sha256
        {
            return Err(unavailable(
                "attribute_catalog answered for another generation",
            ));
        }
        let mut entries = BTreeMap::new();
        for entry in reply["entries"]
            .as_array()
            .ok_or_else(|| unavailable("attribute_catalog's reply has no entries"))?
        {
            let id = uint(&entry["id"]).map_err(unavailable)?;
            entries.insert(id, untag(&entry["descriptor"])?);
        }
        let layouts = reply["layouts"]
            .as_array()
            .ok_or_else(|| unavailable("attribute_catalog's reply has no layouts"))?
            .iter()
            .map(untag)
            .collect::<Result<Vec<_>, _>>()?;
        open.catalog = Catalog::new(entries, layouts);
        Ok(open)
    }

    /// attribute_catalog's reply: a sizing call, then the read into exactly that many bytes.
    fn catalog_bytes(open: &Open, member: Member<CatalogFn>) -> Result<Vec<u8>, String> {
        let mut buffer: Vec<u8> = Vec::new();
        for _ in 0..2 {
            let mut record = error_record();
            let record_ptr = open.record_ptr(&mut record);
            let mut required = 0usize;
            let data: *mut c_char = if buffer.is_empty() {
                std::ptr::null_mut()
            } else {
                buffer.as_mut_ptr().cast()
            };
            // SAFETY: the view this VM acquired; `data` is NULL with capacity 0 or writable for its length; required
            // and the record are live and do not overlap it.
            let result = unsafe {
                open.services.call(
                    member,
                    (
                        open.owner,
                        open.view.view(),
                        data,
                        buffer.len(),
                        &raw mut required,
                        record_ptr,
                    ),
                )
            };
            if result == KILN_ERR_BUFFER_TOO_SMALL && buffer.is_empty() {
                if required == 0 || required > CATALOG_MAX {
                    return Err(unavailable(format!("a catalog of {required} bytes")));
                }
                buffer = vec![0u8; required];
                continue;
            }
            if result != KILN_OK {
                return Err(open.failure("attribute_catalog", result, &record));
            }
            if required != buffer.len() {
                return Err(unavailable("attribute_catalog changed its size"));
            }
            return Ok(buffer);
        }
        Err(unavailable("attribute_catalog changed its size"))
    }
}
