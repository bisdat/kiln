// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.archive v1's attribute group (attribute_catalog, attribute_resolve, attribute_evaluate), called as a native
//! consumer does: through the exported early getter and the generated kiln-abi bindings, never the provider's internals.
//!
//! The archive is the synthetic attribute archive (fixtures/m6/startup/attributes/archive.json, written by EML's writer)
//! and the expected answers are ShroudForge 1.7.9's own catalog and program code over the same archive
//! (fixtures/m6/startup/attributes/golden.json, see m6_startup_vm's `attribute_catalog_matches_shroudforge_on_a_synthetic_archive`):
//! every descriptor and layout, as the neutral tagged tree decodes back, key order included; resolve by name and ID
//! with ShroudForge's refusals behind error tokens; each evaluation's output bits and trace.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::archive::STEM;
use kfc::{
    container::{KFCFile, KFCWriter},
    guid::{Guid, ResourceId},
    reflection::{LookupKey, TypeRegistry},
    resource::value::Value,
};
use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_NOT_FOUND, KILN_ERR_REVOKED, KILN_ERR_UNAVAILABLE,
    KILN_OK, KilnStr,
    services::{kiln_caps::*, kiln_eml_archive::*},
};
use kiln_eml::archive_service::{self, ArchiveBacking, Grant, Phase, RegistrySnapshot};
use serde_json::{Value as Json, json};
use std::{
    ffi::c_void,
    fs,
    path::Path,
    rc::Rc,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

const OWNER: u64 = 0x61;
const OTHER: u64 = 0x62;

static NEXT: AtomicU64 = AtomicU64::new(70_000);

fn api() -> &'static KilnEmlArchiveApiV1 {
    let table = archive_service::kiln_eml_archive_service(1);
    assert!(!table.is_null());
    unsafe { &*table }
}

fn grant() -> Grant {
    Grant {
        phase: Phase::Published,
        launch_id: [0x11; 16],
        plan_id: [0x22; 16],
        generation: NEXT.fetch_add(1, Ordering::Relaxed),
        reload_epoch: 5,
        plan_sha256: [0x33; 32],
        archive_sha256: [0x44; 32],
        prepared_owners: Vec::new(),
    }
}

fn spec() -> Json {
    serde_json::from_str(include_str!("fixtures/m6/startup/attributes/archive.json")).unwrap()
}

fn golden() -> Json {
    serde_json::from_str(include_str!("fixtures/m6/startup/attributes/golden.json")).unwrap()
}

/// The synthetic attribute archive's registry and resources (as m6_startup_vm builds them).
fn attribute_archive() -> (TypeRegistry, Vec<(ResourceId, Vec<u8>)>) {
    let spec = spec();
    let registry: TypeRegistry = serde_json::from_value(spec["registry"].clone()).unwrap();
    let resources = spec["resources"]
        .as_array()
        .unwrap()
        .iter()
        .map(|resource| {
            let ty = registry
                .get_by_name(LookupKey::Qualified(resource["type"].as_str().unwrap()))
                .unwrap();
            let value: Value = serde_json::from_value(resource["value"].clone()).unwrap();
            let bytes = value.to_bytes(&registry, ty).unwrap();
            let guid = Guid::parse(resource["guid"].as_str().unwrap()).unwrap();
            (ResourceId::new(guid, ty.qualified_hash, 0), bytes)
        })
        .collect();
    (registry, resources)
}

/// A game folder holding `resources` over `registry`, with its baseline (the reference catalog).
fn game_with(tag: &str, registry: &TypeRegistry, resources: &[(ResourceId, Vec<u8>)]) -> fixture::TestRoot {
    let root = fixture::TestRoot::new(tag);
    let game = root.0.join("game");
    fs::create_dir_all(&game).unwrap();
    fs::File::create(game.join(format!("{STEM}.kfc"))).unwrap();
    fs::File::create(game.join(format!("{STEM}.kfc_resources"))).unwrap();
    let types = Rc::new(registry.clone());
    let mut writer = KFCWriter::<Rc<KFCFile>, _>::new(&game, STEM, types, "s-1").unwrap();
    for (id, bytes) in resources {
        writer.write_resource(id, bytes).unwrap();
    }
    writer.finalize().unwrap();
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(game.join(format!("{STEM}.{ext}")), game.join(format!("{STEM}.{ext}.bak"))).unwrap();
    }
    root
}

/// A PUBLISHED generation of `dir`'s archive (no transaction unless given) and OWNER's view of it.
fn generation(dir: &Path, registry: TypeRegistry, transaction: Vec<(ResourceId, Vec<u8>)>) -> (KilnEmlArchiveIdentityV1, u64) {
    let backing = ArchiveBacking::open(dir, STEM, transaction).unwrap();
    let registry = Arc::new(registry);
    let snapshot = RegistrySnapshot::new(&registry, backing.game_version());
    let pins = archive_service::authorize_with_archive(grant(), snapshot, registry, Arc::new(backing)).unwrap();
    (pins, acquire(OWNER, &pins))
}

fn acquire(owner: u64, pins: &KilnEmlArchiveIdentityV1) -> u64 {
    let mut view = 0;
    assert_eq!(unsafe { api().acquire.unwrap()(owner, pins, &mut view) }, KILN_OK);
    view
}

/// A refused call's record (Debug for the test asserts).
struct Failure(KilnArchiveErrorV1);
impl std::fmt::Debug for Failure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "result {} token {} message {}", self.0.result, self.0.token, self.0.message_bytes)
    }
}
impl std::ops::Deref for Failure {
    type Target = KilnArchiveErrorV1;
    fn deref(&self) -> &KilnArchiveErrorV1 {
        &self.0
    }
}

fn error_out() -> KilnArchiveErrorV1 {
    KilnArchiveErrorV1 { size: 32, result: KILN_OK, token: 0, message_bytes: 0, flags: 0, reserved: 0 }
}

/// A failed call's result and its retained text (copied, then released once).
fn cause(owner: u64, record: &KilnArchiveErrorV1) -> (i32, String) {
    assert_ne!(record.token, 0, "a genuine failure carries a token");
    let mut required = 0usize;
    let copy = api().error_copy.unwrap();
    assert_eq!(unsafe { copy(owner, record.token, std::ptr::null_mut(), 0, &mut required) }, KILN_ERR_BUFFER_TOO_SMALL);
    let mut text = vec![0u8; required];
    assert_eq!(unsafe { copy(owner, record.token, text.as_mut_ptr().cast(), required, &mut required) }, KILN_OK);
    assert_eq!(unsafe { api().error_release.unwrap()(owner, record.token) }, KILN_OK);
    (record.result, String::from_utf8(text).unwrap())
}

/// A sized JSON call: the probe gives the complete length, the copy exactly that many bytes.
fn sized(call: impl Fn(*mut i8, usize, *mut usize, *mut KilnArchiveErrorV1) -> i32) -> Result<Vec<u8>, Failure> {
    let mut record = error_out();
    let mut required = 0usize;
    let rc = call(std::ptr::null_mut(), 0, &mut required, &mut record);
    if rc != KILN_ERR_BUFFER_TOO_SMALL {
        assert_eq!(record.result, rc);
        return Err(Failure(record));
    }
    assert_eq!((record.token, record.message_bytes), (0, 0), "a capacity refusal has no token");
    let mut bytes = vec![0u8; required];
    let mut written = 0usize;
    let rc = call(bytes.as_mut_ptr().cast(), bytes.len(), &mut written, &mut record);
    assert_eq!(rc, KILN_OK);
    assert_eq!(written, required);
    Ok(bytes)
}

fn catalog(owner: u64, view: u64) -> Result<Json, Failure> {
    sized(|json, capacity, required, error| unsafe {
        api().attribute_catalog.unwrap()(owner, view, json, capacity, required, error)
    })
    .map(|bytes| serde_json::from_slice(&bytes).unwrap())
}

fn kstr(text: &str) -> KilnStr {
    KilnStr { ptr: text.as_ptr().cast(), len: text.len() }
}

fn resolve(owner: u64, view: u64, selector: &str) -> Result<Json, Failure> {
    sized(|json, capacity, required, error| unsafe {
        api().attribute_resolve.unwrap()(owner, view, kstr(selector), json, capacity, required, error)
    })
    .map(|bytes| serde_json::from_slice(&bytes).unwrap())
}

/// One evaluation: the output bits and the reply JSON, or the failure record.
fn evaluate(owner: u64, view: u64, root: u32, input: &[u32]) -> Result<(Vec<u32>, Json), Failure> {
    let mut output = vec![0xdead_beef_u32; input.len()];
    let mut record = error_out();
    let mut required = 0usize;
    let call = api().attribute_evaluate.unwrap();
    let rc = unsafe {
        call(owner, view, root, input.as_ptr(), input.len() as u32, output.as_mut_ptr(), std::ptr::null_mut(), 0, &mut required, &mut record)
    };
    if rc != KILN_ERR_BUFFER_TOO_SMALL {
        assert_eq!(record.result, rc);
        return Err(Failure(record));
    }
    assert!(output.iter().all(|bits| *bits == 0xdead_beef), "a short trace leaves every output untouched");
    let mut trace = vec![0u8; required];
    let rc = unsafe {
        call(owner, view, root, input.as_ptr(), input.len() as u32, output.as_mut_ptr(), trace.as_mut_ptr().cast(), trace.len(), &mut required, &mut record)
    };
    assert_eq!(rc, KILN_OK);
    assert_eq!(required, trace.len());
    Ok((output, serde_json::from_slice(&trace).unwrap()))
}

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap()).collect()
}

/// The tagged tree back as JSON (the inverse the MIT consumer implements); map keys keep their order.
fn untag(node: &Json) -> Json {
    let value = &node["value"];
    match node["tag"].as_str().unwrap() {
        "nil" => Json::Null,
        "bool" => value.clone(),
        "u64" => json!(u64::from_str_radix(value.as_str().unwrap().strip_prefix("u64:").unwrap(), 16).unwrap()),
        "i64" => json!(u64::from_str_radix(value.as_str().unwrap().strip_prefix("i64:").unwrap(), 16).unwrap() as i64),
        "f64" => json!(f64::from_bits(u64::from_str_radix(value.as_str().unwrap().strip_prefix("f64:").unwrap(), 16).unwrap())),
        "string" => json!(String::from_utf8(unhex(value.as_str().unwrap().strip_prefix("string:").unwrap())).unwrap()),
        "array" => Json::Array(node["items"].as_array().unwrap().iter().map(untag).collect()),
        "map" => {
            let mut map = serde_json::Map::new();
            for entry in node["entries"].as_array().unwrap() {
                let Json::String(key) = untag(&entry["key"]) else { panic!("a map key is a string") };
                map.insert(key, untag(&entry["value"]));
            }
            Json::Object(map)
        }
        other => panic!("unexpected tag {other}"),
    }
}

fn keys(value: &Json) -> Vec<String> {
    value.as_object().unwrap().keys().cloned().collect()
}

fn pin_matches(reply: &Json, pins: &KilnEmlArchiveIdentityV1) {
    assert_eq!(reply["schema_version"], 1);
    assert_eq!(reply["pin"]["generation"], format!("u64:{:016x}", pins.generation));
    assert_eq!(reply["pin"]["registry_id"], format!("u64:{:016x}", pins.registry_id));
}

// The catalog is ShroudForge's: every entry in ID order with its whole descriptor (key order kept), its identity
// indexes, its definition GUID when one component owns it, and the reflected layouts; the reply is materialized once.
#[test]
fn the_catalog_is_shroudforges_on_the_synthetic_archive() {
    let (registry, resources) = attribute_archive();
    let root = game_with("g30a-catalog", &registry, &resources);
    let (pins, view) = generation(root.0.join("game").as_std_path(), registry, Vec::new());
    let ours = catalog(OWNER, view).unwrap();
    pin_matches(&ours, &pins);
    let golden = golden();
    let expected = &golden["get_attributes"];
    let entries = ours["entries"].as_array().unwrap();
    let mut by_id: Vec<&Json> = expected["entries"].as_array().unwrap().iter().collect();
    by_id.sort_by_key(|d| d["hash"].as_u64());
    assert_eq!(entries.len(), by_id.len());
    assert_eq!(entries.len(), 10);
    for (entry, descriptor) in entries.iter().zip(by_id) {
        let decoded = untag(&entry["descriptor"]);
        assert_eq!(&decoded, descriptor, "{}", descriptor["name"]);
        assert_eq!(keys(&decoded), keys(descriptor), "key order of {}", descriptor["name"]);
        assert_eq!(entry["id"], descriptor["hash"]);
        assert_eq!(entry["name"], descriptor["name"]);
        assert_eq!(entry["root_hash"], descriptor["root_hash"]);
        assert_eq!(entry["index"], descriptor["index"]);
        assert_eq!(entry["scalar_type"], descriptor["scalar_type"]);
        let components = descriptor["components"].as_array().unwrap();
        let expected_guid = match components.as_slice() {
            [one] => one["definition_guid"].as_str().map(|text| {
                let guid = Guid::parse(text).unwrap();
                format!("guid:{}", guid.data().iter().map(|b| format!("{b:02x}")).collect::<String>())
            }),
            _ => None,
        };
        assert_eq!(entry["definition_guid"].as_str().map(str::to_owned), expected_guid, "{}", descriptor["name"]);
    }
    let layouts: Vec<Json> = ours["layouts"].as_array().unwrap().iter().map(untag).collect();
    assert_eq!(&Json::Array(layouts), &expected["component_layouts"]);
    // materialized once: the same bytes again
    let again = sized(|json, capacity, required, error| unsafe {
        api().attribute_catalog.unwrap()(OWNER, view, json, capacity, required, error)
    })
    .unwrap();
    assert_eq!(serde_json::from_slice::<Json>(&again).unwrap(), ours);
}

// resolve: by original name and by attribute ID; ShroudForge's refusals with their texts behind a token; selectors
// outside the schema refused before any lookup.
#[test]
fn resolve_selects_by_name_and_id_with_shroudforges_refusals() {
    let (registry, resources) = attribute_archive();
    let root = game_with("g30a-resolve", &registry, &resources);
    let (pins, view) = generation(root.0.join("game").as_std_path(), registry, Vec::new());
    let golden = golden();
    for name in ["Stamina", "Stamina_Min", "Revive_Delay", "Mana"] {
        let reply = resolve(OWNER, view, &json!({"schema_version":1,"name":name}).to_string()).unwrap();
        pin_matches(&reply, &pins);
        assert_eq!(untag(&reply["entry"]["descriptor"]), golden["get_attribute"][name]["ok"], "{name}");
        let id = reply["entry"]["id"].as_u64().unwrap();
        let by_id = resolve(OWNER, view, &json!({"schema_version":1,"id":id}).to_string()).unwrap();
        assert_eq!(by_id["entry"], reply["entry"]);
    }
    let refused = |selector: Json| cause(OWNER, &resolve(OWNER, view, &selector.to_string()).unwrap_err());
    assert_eq!(
        refused(json!({"schema_version":1,"name":"Shared"})),
        (KILN_ERR_UNAVAILABLE, golden["get_attribute"]["Shared"]["err"].as_str().unwrap().to_owned())
    );
    assert_eq!(
        refused(json!({"schema_version":1,"name":"Missing"})),
        (KILN_ERR_NOT_FOUND, "attribute not found: Missing".to_owned())
    );
    assert_eq!(refused(json!({"schema_version":1,"id":12345})), (KILN_ERR_NOT_FOUND, "attribute ID not found".to_owned()));
    for bad in [
        json!({"name":"Stamina"}),
        json!({"schema_version":2,"name":"Stamina"}),
        json!({"schema_version":1,"name":"Stamina","id":1}),
        json!({"schema_version":1,"id":4294967296u64}),
        json!({"schema_version":1,"id":-1}),
        json!(["Stamina"]),
    ] {
        let record = resolve(OWNER, view, &bad.to_string()).unwrap_err();
        assert_eq!((record.result, record.token), (KILN_ERR_INVALID_ARGUMENT, 0), "{bad}");
    }
    let record = resolve(OWNER, view, "{not json").unwrap_err();
    assert_eq!((record.result, record.token), (KILN_ERR_INVALID_ARGUMENT, 0));
}

// evaluate: ShroudForge's recalculation of a complete root over the caller's bits (output bits and the ordered trace,
// descending root index); a short trace buffer leaves the outputs untouched; the refusals keep ShroudForge's texts.
#[test]
fn evaluate_recalculates_a_root_as_shroudforge_does() {
    let (registry, resources) = attribute_archive();
    let root = game_with("g30a-evaluate", &registry, &resources);
    let (pins, view) = generation(root.0.join("game").as_std_path(), registry, Vec::new());
    let golden = golden();
    let entries = golden["get_attributes"]["entries"].as_array().unwrap().clone();
    let mut checked = 0;
    for (case, expected) in golden["evaluate"].as_object().unwrap() {
        let Some(ok) = expected["result"].get("ok") else { continue };
        let root_hash = ok["root_hash"].as_u64().unwrap() as u32;
        let mut members: Vec<&Json> = entries.iter().filter(|e| e["root_hash"] == root_hash).collect();
        members.sort_by_key(|e| e["index"].as_u64());
        let input: Vec<u32> = members
            .iter()
            .map(|e| expected["bits"][e["name"].as_str().unwrap()].as_u64().unwrap() as u32)
            .collect();
        let (output, reply) = evaluate(OWNER, view, root_hash, &input).unwrap();
        pin_matches(&reply, &pins);
        assert_eq!(reply["root_hash"], root_hash);
        assert_eq!(reply["input_bits"], json!(input));
        assert_eq!(reply["output_bits"], json!(output));
        let trace: Vec<Json> = reply["trace"].as_array().unwrap().iter().map(untag).collect();
        assert_eq!(Json::Array(trace.clone()), ok["trace"], "{case}");
        assert_eq!(json!(output), trace.last().map_or(json!(input), |t| t["after_bits"].clone()), "{case}");
        checked += 1;
    }
    assert_eq!(checked, 5);
    let stamina = golden["get_attribute"]["Stamina"]["ok"]["root_hash"].as_u64().unwrap() as u32;
    let refused = |root: u32, input: &[u32]| cause(OWNER, &evaluate(OWNER, view, root, input).unwrap_err());
    assert_eq!(refused(stamina, &[1, 2]), (KILN_ERR_INVALID_ARGUMENT, "attribute root value count mismatch".to_owned()));
    assert_eq!(refused(0x1234_5678, &[1]), (KILN_ERR_NOT_FOUND, "incomplete attribute root".to_owned()));
    // null inputs with a nonzero count, overlapping buffers: refused before any lookup
    let mut record = error_out();
    let mut required = 0usize;
    let mut output = [0u32; 3];
    let rc = unsafe {
        api().attribute_evaluate.unwrap()(OWNER, view, stamina, std::ptr::null(), 3, output.as_mut_ptr(), std::ptr::null_mut(), 0, &mut required, &mut record)
    };
    assert_eq!((rc, record.token), (KILN_ERR_INVALID_ARGUMENT, 0));
    let mut both = [1u32, 2, 3];
    let rc = unsafe {
        api().attribute_evaluate.unwrap()(OWNER, view, stamina, both.as_ptr(), 3, both.as_mut_ptr(), std::ptr::null_mut(), 0, &mut required, &mut record)
    };
    assert_eq!((rc, record.token), (KILN_ERR_INVALID_ARGUMENT, 0));
}

// Owners, archives and failed catalogs: another owner's view is REVOKED, a generation without its archive UNAVAILABLE
// (no token), an archive without attribute definitions answers ShroudForge's catalog failure to all three members.
#[test]
fn owners_missing_archives_and_failed_catalogs() {
    let (registry, resources) = attribute_archive();
    let root = game_with("g30a-owners", &registry, &resources);
    let (_, view) = generation(root.0.join("game").as_std_path(), registry.clone(), Vec::new());
    assert_eq!(catalog(OTHER, view).unwrap_err().result, KILN_ERR_REVOKED);
    let other_view = acquire(OTHER, &{
        let backing = ArchiveBacking::open(root.0.join("game").as_std_path(), STEM, Vec::new()).unwrap();
        let registry = Arc::new(registry.clone());
        let snapshot = RegistrySnapshot::new(&registry, backing.game_version());
        archive_service::authorize_with_archive(grant(), snapshot, registry, Arc::new(backing)).unwrap()
    });
    assert!(catalog(OTHER, other_view).is_ok());
    // no archive
    let snapshot = RegistrySnapshot::new(&registry, "g09c-attributes");
    let bare = archive_service::authorize_with_registry(grant(), snapshot, Arc::new(registry.clone())).unwrap();
    let bare_view = acquire(OWNER, &bare);
    let record = catalog(OWNER, bare_view).unwrap_err();
    assert_eq!((record.result, record.token), (KILN_ERR_UNAVAILABLE, 0));
    // an archive whose reference catalog holds no attribute definitions (only the components' registry)
    let empty = game_with("g30a-empty", &registry, &[]);
    let (_, empty_view) = generation(empty.0.join("game").as_std_path(), registry.clone(), Vec::new());
    let text = "no attribute definitions in this KFC container".to_owned();
    assert_eq!(cause(OWNER, &catalog(OWNER, empty_view).unwrap_err()), (KILN_ERR_UNAVAILABLE, text.clone()));
    let selector = json!({"schema_version":1,"name":"Stamina"}).to_string();
    assert_eq!(cause(OWNER, &resolve(OWNER, empty_view, &selector).unwrap_err()), (KILN_ERR_UNAVAILABLE, text.clone()));
    assert_eq!(cause(OWNER, &evaluate(OWNER, empty_view, 1, &[0]).unwrap_err()), (KILN_ERR_UNAVAILABLE, text));
    // the catalog reads the REFERENCE catalog only: a transaction that adds a definition does not change it
    let (_, staged_view) = generation(empty.0.join("game").as_std_path(), registry, resources);
    assert_eq!(catalog(OWNER, staged_view).unwrap_err().result, KILN_ERR_UNAVAILABLE);
}

// The group is in the table with its rows: AVAILABLE with its three member rows.
#[test]
fn the_attribute_group_is_published_with_its_rows() {
    let table = api();
    assert!(table.attribute_catalog.is_some() && table.attribute_resolve.is_some() && table.attribute_evaluate.is_some());
    static ROWS: Mutex<Vec<(String, u32)>> = Mutex::new(Vec::new());
    unsafe extern "C" fn report(_: u64, name: KilnStr, state: u32, _: u32, _: KilnStr) -> i32 {
        let name = unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) };
        ROWS.lock().unwrap().push((String::from_utf8_lossy(name).into_owned(), state));
        KILN_OK
    }
    let mut caps: KilnCapsApi = unsafe { std::mem::zeroed() };
    caps.header.size = std::mem::size_of::<KilnCapsApi>() as u32;
    caps.header.version = 1;
    caps.report = Some(report);
    assert_eq!(archive_service::publish(0x63, |_: &str, _: *const c_void| KILN_OK, Some(&caps)), KILN_OK);
    let rows = ROWS.lock().unwrap();
    for name in [
        "eml.archive.attributes",
        "eml.archive.attributes.attribute_catalog",
        "eml.archive.attributes.attribute_resolve",
        "eml.archive.attributes.attribute_evaluate",
    ] {
        assert!(rows.iter().any(|(n, s)| n == name && *s == KILN_CAP_AVAILABLE as u32), "{name}");
    }
}
