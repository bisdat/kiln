// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.archive v1's metadata subset (acquire, identity, release, registry_metadata) and its per-call error
//! channel (error_copy, error_release), called as a native consumer does: through the exported early getter and the
//! generated kiln-abi bindings of native/core/abi/services/kiln_eml_archive.h (clang-measured layout), never through
//! the provider's Rust internals. The startup controller side (authorize/retire/revoke_owner) is the GPL Rust API the startup controller and the plugin
//! call. The registry is the type session's synthetic one (tests/support/m6_types).
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_types/mod.rs"]
mod reg;

use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_REVOKED, KILN_ERR_UNAVAILABLE,
    KILN_OK, KilnStr,
    services::{kiln_caps::*, kiln_eml_archive::*},
};
use kiln_eml::archive_service::{self, Grant, Phase, RegistrySnapshot};
use std::{
    collections::BTreeMap,
    ffi::c_void,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

/// Generations and owners are process-global in the provider: every test takes its own.
static NEXT: AtomicU64 = AtomicU64::new(1_000);
fn next() -> u64 {
    NEXT.fetch_add(1, Ordering::Relaxed)
}

fn api() -> &'static KilnEmlArchiveApiV1 {
    let table = archive_service::kiln_eml_archive_service(1);
    assert!(!table.is_null());
    unsafe { &*table }
}

fn snapshot() -> Arc<RegistrySnapshot> {
    RegistrySnapshot::new(&reg::registry(), "m6-kfc-data-7")
}

fn grant(phase: Phase, generation: u64) -> Grant {
    Grant {
        phase,
        launch_id: [0x11; 16],
        plan_id: [0x22; 16],
        generation,
        reload_epoch: 1,
        plan_sha256: [0x33; 32],
        archive_sha256: [0x44; 32],
        prepared_owners: Vec::new(),
    }
}

/// An authorized PUBLISHED generation over the type session's registry and its identity pins.
fn published() -> (KilnEmlArchiveIdentityV1, Arc<RegistrySnapshot>) {
    let snap = snapshot();
    let pins = archive_service::authorize(grant(Phase::Published, next()), snap.clone()).unwrap();
    (pins, snap)
}

fn acquire(owner: u64, pins: &KilnEmlArchiveIdentityV1) -> Result<u64, i32> {
    let mut view = 0;
    match unsafe { api().acquire.unwrap()(owner, pins, &mut view) } {
        KILN_OK => Ok(view),
        rc => {
            assert_eq!(view, 0, "a refused acquire leaves its output alone");
            Err(rc)
        }
    }
}

fn error_out() -> KilnArchiveErrorV1 {
    KilnArchiveErrorV1 {
        size: 32,
        result: -1,
        token: u64::MAX,
        message_bytes: u64::MAX,
        flags: 0,
        reserved: 0,
    }
}

/// What a failed call left in its error record.
#[derive(Debug, Clone, Copy)]
struct Failure {
    result: i32,
    token: u64,
    message_bytes: u64,
}

/// registry_metadata sized then copied; Err is (result, the error record).
fn page(view: u64, first: u64, limit: u32) -> Result<serde_json::Value, (i32, Failure)> {
    let call = api().registry_metadata.unwrap();
    let mut e = error_out();
    let mut required = 0usize;
    let rc = unsafe { call(view, first, limit, std::ptr::null_mut(), 0, &mut required, &mut e) };
    if rc != KILN_ERR_BUFFER_TOO_SMALL {
        assert_ne!(rc, KILN_OK, "a JSON reply is never empty");
        return Err((
            rc,
            Failure {
                result: e.result,
                token: e.token,
                message_bytes: e.message_bytes,
            },
        ));
    }
    assert_eq!(
        (e.result, e.token, e.message_bytes),
        (KILN_ERR_BUFFER_TOO_SMALL, 0, 0),
        "capacity refusal has no token"
    );
    let mut text = vec![0u8; required];
    let mut again = 0usize;
    let rc = unsafe {
        call(
            view,
            first,
            limit,
            text.as_mut_ptr().cast(),
            text.len(),
            &mut again,
            &mut e,
        )
    };
    if rc != KILN_OK {
        // the view was closed between the two calls (the concurrency test retires it)
        assert_eq!((rc, e.result, e.token), (KILN_ERR_REVOKED, KILN_ERR_REVOKED, 0));
        return Err((
            rc,
            Failure {
                result: e.result,
                token: e.token,
                message_bytes: e.message_bytes,
            },
        ));
    }
    assert_eq!((again, e.result, e.token), (required, KILN_OK, 0));
    Ok(serde_json::from_slice(&text).expect("UTF-8 JSON without a NUL"))
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn u64s(v: u64) -> String {
    format!("u64:{v:016x}")
}

// The table: version 1, 152 bytes, the metadata subset, the error channel and the read group non-null,
// every codec and attribute slot null; getter versions 0 and 1 give the same process-lifetime table, 2 gives none.
#[test]
fn the_early_table_has_only_the_metadata_subset_and_error_channel() {
    let table = api();
    assert_eq!(
        (table.header.size, table.header.version),
        (152, KILN_EML_ARCHIVE_VERSION)
    );
    assert_eq!(archive_service::kiln_eml_archive_service(0), table as *const _);
    assert!(archive_service::kiln_eml_archive_service(2).is_null());
    assert!(table.acquire.is_some() && table.identity.is_some() && table.release.is_some());
    assert!(table.registry_metadata.is_some());
    assert!(table.error_copy.is_some() && table.error_release.is_some());
    // The read group (m6_archive_service tests it)
    assert!(table.resource_json.is_some());
    assert!(table.find_resource.is_some() && table.list_resources.is_some() && table.read_resource.is_some());
    assert!(table.content_read_range.is_some() && table.list_contents.is_some());
    assert!(table.value_decode.is_none() && table.value_encode.is_none() && table.loaded_field.is_none());
    // The attribute group (m6_archive_attributes tests it)
    assert!(table.attribute_catalog.is_some() && table.attribute_resolve.is_some());
    assert!(table.attribute_evaluate.is_some());
    // A 48-byte metadata-only reader's view of the same table.
    assert!(table.header.size >= KILN_EML_ARCHIVE_REGISTRY_SUBSET_SIZE);
}

// Getter presence grants nothing: no authorized generation means UNAVAILABLE, never an empty view.
#[test]
fn acquire_needs_an_authorized_generation_with_every_pin() {
    let snap = snapshot();
    let mut pins = archive_service::authorize(grant(Phase::Published, next()), snap.clone()).unwrap();
    assert_eq!(pins.size, 160);
    assert_eq!(pins.registry_id, snap.id());
    assert_eq!(pins.registry_sha256, *snap.sha256());
    assert_ne!(pins.registry_id, 0);
    let owner = next();
    let view = acquire(owner, &pins).unwrap();
    assert_ne!(view, 0);
    // every other pin, one at a time
    type Edit = fn(&mut KilnEmlArchiveIdentityV1);
    let edits: [(&str, Edit); 9] = [
        ("launch", |p| p.launch_id[15] ^= 1),
        ("plan", |p| p.plan_id[0] ^= 1),
        ("generation", |p| p.generation += 1_000_000),
        ("epoch", |p| p.reload_epoch += 1),
        ("plan digest", |p| p.plan_sha256[31] ^= 1),
        ("archive digest (another exe's data)", |p| p.archive_sha256[0] ^= 1),
        ("registry id", |p| p.registry_id += 1),
        ("registry digest", |p| p.registry_sha256[5] ^= 1),
        ("phase", |p| p.phase = KILN_EML_ARCHIVE_PREPARED as u32),
    ];
    for (what, edit) in edits {
        let mut wrong = pins;
        edit(&mut wrong);
        assert_eq!(acquire(owner, &wrong), Err(KILN_ERR_UNAVAILABLE), "{what}");
    }
    // malformed records
    assert_eq!(acquire(0, &pins), Err(KILN_ERR_INVALID_ARGUMENT), "owner 0");
    let mut short = pins;
    short.size = 159;
    assert_eq!(acquire(owner, &short), Err(KILN_ERR_INVALID_ARGUMENT));
    let mut phase = pins;
    phase.phase = 3;
    assert_eq!(acquire(owner, &phase), Err(KILN_ERR_INVALID_ARGUMENT));
    let mut zero = pins;
    zero.generation = 0;
    assert_eq!(
        acquire(owner, &zero),
        Err(KILN_ERR_INVALID_ARGUMENT),
        "no wildcard pins"
    );
    let mut out = 0u64;
    assert_eq!(
        unsafe { api().acquire.unwrap()(owner, std::ptr::null(), &mut out) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { api().acquire.unwrap()(owner, &pins, std::ptr::null_mut()) },
        KILN_ERR_INVALID_ARGUMENT
    );
    let mut record = pins;
    let base = std::ptr::addr_of_mut!(record);
    let inside = unsafe { std::ptr::addr_of_mut!((*base).generation) };
    assert_eq!(
        unsafe { api().acquire.unwrap()(owner, base, inside) },
        KILN_ERR_INVALID_ARGUMENT,
        "the output inside the expected record"
    );
    assert_eq!(record.generation, pins.generation);
    // a larger caller record: its suffix is ignored
    #[repr(C)]
    struct Larger {
        v1: KilnEmlArchiveIdentityV1,
        extra: [u8; 8],
    }
    pins.size = 168;
    let larger = Larger {
        v1: pins,
        extra: [0xee; 8],
    };
    let mut view2 = 0;
    assert_eq!(
        unsafe { api().acquire.unwrap()(owner, &larger.v1, &mut view2) },
        KILN_OK
    );
    assert!(view2 != 0 && view2 != view, "tokens are never reused");
    assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
    assert_eq!(unsafe { api().release.unwrap()(view2) }, KILN_OK);
}

#[test]
fn an_unauthorized_or_retired_generation_is_refused() {
    let snap = snapshot();
    let g = next();
    let mut never = archive_service::authorize(grant(Phase::Published, g), snap.clone()).unwrap();
    never.generation = next();
    assert_eq!(
        acquire(next(), &never),
        Err(KILN_ERR_UNAVAILABLE),
        "a generation nobody authorized"
    );
    let pins = archive_service::authorize(grant(Phase::Published, next()), snap.clone()).unwrap();
    let owner = next();
    let view = acquire(owner, &pins).unwrap();
    archive_service::retire(pins.generation);
    assert_eq!(acquire(owner, &pins), Err(KILN_ERR_REVOKED), "a retired generation");
    assert_eq!(
        page(view, 0, 0).unwrap_err().0,
        KILN_ERR_REVOKED,
        "its views are revoked"
    );
    assert_eq!(
        unsafe { api().release.unwrap()(view) },
        KILN_OK,
        "release stays idempotent after revocation"
    );
    // The same generation number with other pins cannot be authorized twice.
    let mut again = grant(Phase::Published, g);
    again.archive_sha256 = [0x99; 32];
    assert!(archive_service::authorize(again, snap).is_err());
}

// PREPARED grants need the startup controller's authorization of that owner; PUBLISHED ones are runtime grants.
#[test]
fn prepared_generations_admit_only_authorized_owners() {
    let (allowed, other) = (next(), next());
    let mut g = grant(Phase::Prepared, next());
    g.prepared_owners = vec![allowed];
    let pins = archive_service::authorize(g, snapshot()).unwrap();
    assert_eq!(pins.phase, KILN_EML_ARCHIVE_PREPARED as u32);
    assert_eq!(acquire(other, &pins), Err(KILN_ERR_UNAVAILABLE));
    let view = acquire(allowed, &pins).unwrap();
    assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
}

#[test]
fn identity_is_a_copy_of_the_granted_pins() {
    let (pins, _) = published();
    let view = acquire(next(), &pins).unwrap();
    #[repr(C)]
    struct Larger {
        v1: KilnEmlArchiveIdentityV1,
        extra: [u8; 8],
    }
    let mut out = Larger {
        v1: unsafe { std::mem::zeroed() },
        extra: [0xee; 8],
    };
    out.v1.size = 168;
    assert_eq!(unsafe { api().identity.unwrap()(view, &mut out.v1) }, KILN_OK);
    assert_eq!(out.extra, [0xee; 8], "bytes past v1 untouched");
    assert_eq!(out.v1.size, 168, "the caller's size is kept");
    let mut copy = out.v1;
    copy.size = 160;
    assert_eq!(bytes_of(&copy), bytes_of(&pins));
    let mut short: KilnEmlArchiveIdentityV1 = unsafe { std::mem::zeroed() };
    short.size = 8;
    assert_eq!(
        unsafe { api().identity.unwrap()(view, &mut short) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { api().identity.unwrap()(view, std::ptr::null_mut()) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { api().identity.unwrap()(0, &mut copy) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
    copy.size = 160;
    assert_eq!(unsafe { api().identity.unwrap()(view, &mut copy) }, KILN_ERR_REVOKED);
}

fn bytes_of(p: &KilnEmlArchiveIdentityV1) -> Vec<u8> {
    unsafe { std::slice::from_raw_parts((p as *const KilnEmlArchiveIdentityV1).cast::<u8>(), 160) }.to_vec()
}

#[test]
fn release_is_idempotent_for_issued_tokens_only() {
    let (pins, _) = published();
    let view = acquire(next(), &pins).unwrap();
    let release = api().release.unwrap();
    assert_eq!(unsafe { release(view) }, KILN_OK);
    assert_eq!(unsafe { release(view) }, KILN_OK);
    assert_eq!(unsafe { release(0) }, KILN_ERR_INVALID_ARGUMENT);
    assert_eq!(unsafe { release(u64::MAX) }, KILN_ERR_INVALID_ARGUMENT);
    assert_eq!(page(view, 0, 0).unwrap_err().0, KILN_ERR_REVOKED);
    assert_eq!(
        page(u64::MAX - 7, 0, 0).unwrap_err().0,
        KILN_ERR_INVALID_ARGUMENT,
        "an unknown view"
    );
}

#[test]
fn revoking_an_owner_closes_all_its_views() {
    let (pins, _) = published();
    let (owner, other) = (next(), next());
    let a = acquire(owner, &pins).unwrap();
    let b = acquire(owner, &pins).unwrap();
    let c = acquire(other, &pins).unwrap();
    archive_service::revoke_owner(owner);
    for view in [a, b] {
        assert_eq!(page(view, 0, 0).unwrap_err().0, KILN_ERR_REVOKED);
        assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
    }
    assert!(page(c, 0, 0).is_ok(), "another owner's view stays");
    // the revoked owner can acquire again (a new grant, a new token)
    let d = acquire(owner, &pins).unwrap();
    assert!(d > c);
    for view in [c, d] {
        assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
    }
}

// FINAL game.version / game.types.version / count: the header page (limit 0) carries the pins, both version texts
// unchanged, the count and first, and no types.
#[test]
fn the_header_page_has_pins_versions_and_count() {
    let (pins, snap) = published();
    let view = acquire(next(), &pins).unwrap();
    let header = page(view, 0, 0).unwrap();
    assert_eq!(header["schema_version"], 1);
    assert_eq!(header["game_version"], "m6-kfc-data-7");
    assert_eq!(header["registry_version"], fixture::types::VERSION);
    assert_eq!(header["count"], u64s(reg::COUNT as u64));
    assert_eq!(header["first"], u64s(0));
    assert_eq!(header["types"], serde_json::json!([]));
    fixture::save_observation("g10a-registry-header", &header);
    let pin = &header["pin"];
    assert_eq!(pin["launch_id"], "11111111-1111-1111-1111-111111111111");
    assert_eq!(pin["plan_id"], "22222222-2222-2222-2222-222222222222");
    assert_eq!(pin["generation"], u64s(pins.generation));
    assert_eq!(pin["reload_epoch"], u64s(1));
    assert_eq!(pin["plan_sha256"], hex(&[0x33; 32]));
    assert_eq!(pin["archive_sha256"], hex(&[0x44; 32]));
    assert_eq!(pin["registry_id"], u64s(snap.id()));
    assert_eq!(pin["registry_sha256"], hex(snap.sha256()));
    let keys: Vec<&str> = header.as_object().unwrap().keys().map(String::as_str).collect();
    let mut sorted = keys.clone();
    sorted.sort();
    assert_eq!(
        sorted,
        [
            "count",
            "first",
            "game_version",
            "pin",
            "registry_version",
            "schema_version",
            "types"
        ]
    );
    unsafe { api().release.unwrap()(view) };
}

// The two version texts are carried byte for byte, without parsing (both pins' types.rs:42-44, game/mod.rs:13-19).
#[test]
fn version_texts_are_not_normalized() {
    let registry = reg::registry_of("r\u{e9}vision\u{0}\u{3b2}", reg::types());
    let snap = RegistrySnapshot::new(&registry, "000026430625");
    let pins = archive_service::authorize(grant(Phase::Published, next()), snap).unwrap();
    let view = acquire(next(), &pins).unwrap();
    let header = page(view, 0, 0).unwrap();
    assert_eq!(header["game_version"], "000026430625");
    assert_eq!(header["registry_version"], "r\u{e9}vision\u{0}\u{3b2}");
    fixture::save_observation("g10a-registry-versions", &header);
    unsafe { api().release.unwrap()(view) };
}

// FINAL game.types.get_by_index / Type fields: ordered pages concatenate to the whole ordered registry with native
// indices, all four hash domains, defaults as exact bytes (absent is null), u64 enum values, flags, attributes.
#[test]
fn pages_are_lossless_ordered_and_concatenate() {
    let (pins, _) = published();
    let view = acquire(next(), &pins).unwrap();
    // Types 0..BROKEN have resolvable inheritance (BROKEN and the cycle have none: see the failure test).
    let healthy = reg::BROKEN as u32;
    let all = page(view, 0, healthy).unwrap();
    // Raw replies for the out-of-tree schema check (registry.schema.json), when M6_WITNESS_OUT is set.
    fixture::save_observation("g10a-registry-page", &all);
    let all = all["types"].as_array().unwrap().clone();
    assert_eq!(all.len(), healthy as usize);
    for (i, ty) in all.iter().enumerate() {
        assert_eq!(ty["index"], u64s(i as u64), "registry order with native indices");
    }
    let mut joined = Vec::new();
    for first in (0..healthy as u64).step_by(4) {
        let p = page(view, first, 4.min(healthy - first as u32)).unwrap();
        assert_eq!(p["first"], u64s(first));
        joined.extend(p["types"].as_array().unwrap().iter().cloned());
    }
    assert_eq!(joined, all, "pages are slices of one order");
    // a limit past the end selects what is left
    let tail = page(view, reg::COUNT as u64 - 1, 100).unwrap();
    assert_eq!(tail["types"].as_array().unwrap().len(), 1);
    let child = &all[2];
    assert_eq!(child["qualified_name"], "m6.Child");
    assert_eq!(
        child["default_bytes"], "bytes:1100ff0080170000",
        "exact bytes with zeros and non-UTF-8"
    );
    assert_eq!(
        all[0]["default_bytes"],
        serde_json::Value::Null,
        "absent is null, not zeros"
    );
    assert_eq!(child["inner_type"], u64s(1));
    assert_eq!(child["primitive_type"], "Struct");
    assert_eq!(child["attributes"][0]["key"], "tag");
    assert_eq!(child["attributes"][0]["namespace"], serde_json::json!(["m6", "attr"]));
    assert_eq!(child["attributes"][0]["type_index"], u64s(0));
    assert_eq!(child["attributes"][0]["value"], "child-tag");
    assert_eq!(
        child["struct_fields"][0]["attributes"][0]["type_index"],
        serde_json::Value::Null
    );
    // effective fields, base first, with the declaring type
    assert_eq!(child["fields"][0]["name"], "base");
    assert_eq!(child["fields"][0]["declaring_type"], u64s(1));
    assert_eq!(child["fields"][1]["name"], "extra");
    assert_eq!(child["fields"][1]["data_offset"], u64s(4));
    let over = &all[reg::OVERRIDE];
    let names: Vec<_> = over["fields"]
        .as_array()
        .unwrap()
        .iter()
        .map(|f| f["name"].clone())
        .collect();
    assert_eq!(names, ["base", "late"], "a redeclared base field keeps the base's slot");
    let mode = &all[reg::MODE];
    assert_eq!(mode["enum_fields"][2]["name"], "Big");
    assert_eq!(mode["enum_fields"][2]["value"], u64s(reg::BIG_ENUM));
    assert_eq!(mode["enum_fields"][2]["value_hex"], format!("0x{:016x}", reg::BIG_ENUM));
    // hash domains are never collapsed: the shared internal hash stays on both types
    for i in [reg::LAYOUT_A, reg::LAYOUT_B] {
        assert_eq!(all[i]["hashes"]["internal"], reg::INTERNAL_SHARED);
    }
    let aliased = &all[reg::ALIASED];
    assert_eq!(aliased["hashes"]["qualified"], kfc::hash::fnv(reg::GHOST));
    assert_eq!(aliased["qualified_name"], "m6.Aliased");
    // Types after the broken ones are served by pages that skip them.
    let late = page(view, reg::ALIAS as u64, (reg::COUNT - reg::ALIAS) as u32).unwrap();
    fixture::save_observation("g10a-registry-late", &late);
    let tagged = &late["types"][reg::TAGGED - reg::ALIAS];
    assert_eq!(tagged["index"], u64s(reg::TAGGED as u64));
    assert_eq!(
        tagged["flags"],
        serde_json::json!(["HAS_BLOB_STRING", "IS_GPU_UNIFORM"])
    );
    assert_eq!(tagged["has_ds"], false);
    assert_eq!(all[9]["has_ds"], true);
    unsafe { api().release.unwrap()(view) };
}

#[test]
fn page_ranges_follow_the_header_rules() {
    let (pins, _) = published();
    let view = acquire(next(), &pins).unwrap();
    let count = reg::COUNT as u64;
    let end = page(view, count, 5).unwrap();
    assert_eq!(end["types"], serde_json::json!([]), "first==count is empty success");
    assert_eq!(
        page(view, count + 1, 0).unwrap_err().0,
        KILN_ERR_INVALID_ARGUMENT,
        "first>count"
    );
    assert_eq!(
        page(view, u64::MAX, 1).unwrap_err().0,
        KILN_ERR_INVALID_ARGUMENT,
        "first+limit overflows"
    );
    unsafe { api().release.unwrap()(view) };
}

// Buffers: required is mandatory; a short buffer gets the complete count and nothing else; overlap refuses.
#[test]
fn short_buffers_write_nothing_but_required() {
    let (pins, _) = published();
    let view = acquire(next(), &pins).unwrap();
    let call = api().registry_metadata.unwrap();
    let full = serde_json::to_vec(&page(view, 0, 3).unwrap()).unwrap().len();
    let mut e = error_out();
    let mut required = 0usize;
    unsafe { call(view, 0, 3, std::ptr::null_mut(), 0, &mut required, &mut e) };
    assert!(required >= full);
    let mut buf = vec![0x5au8; required];
    let mut again = 0usize;
    let rc = unsafe { call(view, 0, 3, buf.as_mut_ptr().cast(), required - 1, &mut again, &mut e) };
    assert_eq!((rc, again), (KILN_ERR_BUFFER_TOO_SMALL, required));
    assert!(buf.iter().all(|&b| b == 0x5a), "no partial reply");
    assert_eq!(
        unsafe {
            call(
                view,
                0,
                3,
                buf.as_mut_ptr().cast(),
                required,
                std::ptr::null_mut(),
                &mut e,
            )
        },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { call(view, 0, 3, std::ptr::null_mut(), 5, &mut again, &mut e) },
        KILN_ERR_INVALID_ARGUMENT
    );
    // required inside the output buffer
    let overlap = unsafe { buf.as_mut_ptr().add(8).cast::<usize>() };
    let rc = unsafe { call(view, 0, 3, buf.as_mut_ptr().cast(), required, overlap, &mut e) };
    assert_eq!(rc, KILN_ERR_INVALID_ARGUMENT, "outputs may not overlap");
    unsafe { api().release.unwrap()(view) };
}

// The error record: checked before any work, initialized on every return, NULL opts out.
#[test]
fn the_error_record_is_checked_first_and_always_initialized() {
    let (pins, _) = published();
    let view = acquire(next(), &pins).unwrap();
    let call = api().registry_metadata.unwrap();
    let mut required = 0usize;
    let closed = acquire(next(), &pins).unwrap();
    assert_eq!(unsafe { api().release.unwrap()(closed) }, KILN_OK);
    for (what, edit) in [
        (
            "short",
            (|e: &mut KilnArchiveErrorV1| e.size = 31) as fn(&mut KilnArchiveErrorV1),
        ),
        ("flags", |e| e.flags = 1),
        ("reserved", |e| e.reserved = 1),
    ] {
        let mut e = error_out();
        edit(&mut e);
        // a released view would answer REVOKED: an invalid record refuses before that lookup
        let rc = unsafe { call(closed, 0, 0, std::ptr::null_mut(), 0, &mut required, &mut e) };
        assert_eq!(rc, KILN_ERR_INVALID_ARGUMENT, "{what}");
        assert_eq!(
            (e.token, e.message_bytes),
            (u64::MAX, u64::MAX),
            "{what}: an invalid record is not written"
        );
    }
    // a valid larger record: v1 initialized, the suffix kept
    #[repr(C)]
    struct Larger {
        v1: KilnArchiveErrorV1,
        extra: [u8; 8],
    }
    let mut e = Larger {
        v1: error_out(),
        extra: [0xee; 8],
    };
    e.v1.size = 40;
    let rc = unsafe { call(view, u64::MAX, 1, std::ptr::null_mut(), 0, &mut required, &mut e.v1) };
    assert_eq!(rc, KILN_ERR_INVALID_ARGUMENT);
    assert_eq!(
        (e.v1.size, e.v1.result, e.v1.token, e.v1.message_bytes),
        (40, KILN_ERR_INVALID_ARGUMENT, 0, 0)
    );
    assert_eq!(e.extra, [0xee; 8]);
    // Overlapping outputs refuse: a valid record of its own is initialized, one that overlaps an output is not
    // written at all.
    let mut buf = vec![0x5au8; 256];
    let inside = unsafe { buf.as_mut_ptr().add(64).cast::<usize>() };
    let mut e = error_out();
    let rc = unsafe { call(view, 0, 3, buf.as_mut_ptr().cast(), buf.len(), inside, &mut e) };
    assert_eq!(rc, KILN_ERR_INVALID_ARGUMENT);
    assert_eq!(
        (e.result, e.token, e.message_bytes),
        (KILN_ERR_INVALID_ARGUMENT, 0, 0),
        "required inside json: the separate record is initialized"
    );
    assert!(buf.iter().all(|&b| b == 0x5a), "nothing else is written");
    let record = unsafe { buf.as_mut_ptr().add(128).cast::<KilnArchiveErrorV1>() };
    unsafe { record.write_unaligned(error_out()) };
    let rc = unsafe { call(view, 0, 3, buf.as_mut_ptr().cast(), buf.len(), &mut required, record) };
    assert_eq!(rc, KILN_ERR_INVALID_ARGUMENT);
    let after = unsafe { record.read_unaligned() };
    assert_eq!(
        (after.result, after.token, after.message_bytes),
        (-1, u64::MAX, u64::MAX),
        "a record inside json is not written"
    );
    unsafe { api().release.unwrap()(view) };
    let mut e = error_out();
    assert_eq!(
        unsafe { call(view, 0, 0, std::ptr::null_mut(), 0, &mut required, &mut e) },
        KILN_ERR_REVOKED
    );
    assert_eq!(
        (e.result, e.token, e.message_bytes),
        (KILN_ERR_REVOKED, 0, 0),
        "a stale grant is no provider failure"
    );
    let rc = unsafe { call(view, 0, 0, std::ptr::null_mut(), 0, &mut required, std::ptr::null_mut()) };
    assert_eq!(rc, KILN_ERR_REVOKED, "NULL opts out of detail");
}

// A real provider failure (a type whose inheritance cannot be resolved, so its effective fields cannot be listed)
// returns INTERNAL with a fresh token holding the complete original cause; copy/release follow the header.
#[test]
fn provider_failures_retain_their_cause_behind_fresh_tokens() {
    let (pins, _) = published();
    let owner = next();
    let view = acquire(owner, &pins).unwrap();
    let (rc, e) = page(view, reg::BROKEN as u64, 1).unwrap_err();
    assert_eq!((rc, e.result), (KILN_ERR_INTERNAL, KILN_ERR_INTERNAL));
    assert!(e.token != 0 && e.message_bytes > 0);
    let (_, second) = page(view, reg::CYCLE_A as u64, 2).unwrap_err();
    assert!(
        second.token != 0 && second.token != e.token,
        "each failing call has its own token"
    );
    let copy = api().error_copy.unwrap();
    let release = api().error_release.unwrap();
    let mut required = 0usize;
    assert_eq!(
        unsafe { copy(owner, e.token, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_BUFFER_TOO_SMALL
    );
    assert_eq!(required as u64, e.message_bytes);
    let mut text = vec![b'z'; required];
    let rc = unsafe { copy(owner, e.token, text.as_mut_ptr().cast(), required - 1, &mut required) };
    assert_eq!(rc, KILN_ERR_BUFFER_TOO_SMALL);
    assert!(text.iter().all(|&b| b == b'z'), "a short copy writes nothing");
    assert_eq!(
        unsafe { copy(owner, e.token, text.as_mut_ptr().cast(), text.len(), &mut required) },
        KILN_OK
    );
    let text = String::from_utf8(text).unwrap();
    assert!(
        text.contains("m6.Broken") && text.contains("unresolved base type"),
        "{text}"
    );
    let mut cycle = vec![0u8; second.message_bytes as usize];
    unsafe {
        copy(
            owner,
            second.token,
            cycle.as_mut_ptr().cast(),
            cycle.len(),
            &mut required,
        )
    };
    assert!(String::from_utf8(cycle).unwrap().contains("cyclic type inheritance"));
    // owners, unknown tokens, release
    assert_eq!(
        unsafe { copy(owner + 1, e.token, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_REVOKED
    );
    assert_eq!(
        unsafe { copy(owner, 0, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { copy(owner, u64::MAX, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { copy(owner, e.token, std::ptr::null_mut(), 0, std::ptr::null_mut()) },
        KILN_ERR_INVALID_ARGUMENT
    );
    let mut both = vec![b'z'; e.message_bytes as usize + 16];
    let inside = unsafe { both.as_mut_ptr().add(8).cast::<usize>() };
    assert_eq!(
        unsafe { copy(owner, e.token, both.as_mut_ptr().cast(), both.len(), inside) },
        KILN_ERR_INVALID_ARGUMENT,
        "required inside the output"
    );
    assert!(both.iter().all(|&b| b == b'z'));
    assert_eq!(unsafe { release(owner + 1, e.token) }, KILN_ERR_REVOKED);
    assert_eq!(unsafe { release(owner, e.token) }, KILN_OK);
    assert_eq!(unsafe { release(owner, e.token) }, KILN_OK, "idempotent");
    assert_eq!(
        unsafe { copy(owner, e.token, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_REVOKED
    );
    assert_eq!(unsafe { release(owner, 0) }, KILN_ERR_INVALID_ARGUMENT);
    // the view's retirement reclaims its tokens; release stays idempotent
    assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
    assert_eq!(
        unsafe { copy(owner, second.token, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_REVOKED
    );
    assert_eq!(unsafe { release(owner, second.token) }, KILN_OK);
}

// Calls pin their generation: a release or retirement during in-flight work never frees what that work reads, and
// tokens stay unique across threads.
#[test]
fn concurrent_reads_releases_and_retirement_are_safe() {
    let snap = snapshot();
    let pins = archive_service::authorize(grant(Phase::Published, next()), snap).unwrap();
    let seen = Arc::new(Mutex::new(Vec::new()));
    let threads: Vec<_> = (0..8)
        .map(|t| {
            let seen = seen.clone();
            std::thread::spawn(move || {
                let owner = next();
                for i in 0..200 {
                    let Ok(view) = acquire(owner, &pins) else { break };
                    seen.lock().unwrap().push(view);
                    match page(view, (i % 10) as u64, 3) {
                        Ok(p) => assert_eq!(p["types"].as_array().unwrap().len(), 3),
                        Err((rc, _)) => assert_eq!(rc, KILN_ERR_REVOKED),
                    }
                    if t % 2 == 0 {
                        assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
                    }
                }
            })
        })
        .collect();
    // Retire while the threads are mid-way, not after they finished.
    while seen.lock().unwrap().len() < 400 {
        std::thread::yield_now();
    }
    archive_service::retire(pins.generation);
    for t in threads {
        t.join().unwrap();
    }
    let mut views = seen.lock().unwrap().clone();
    let n = views.len();
    views.sort();
    views.dedup();
    assert_eq!(views.len(), n, "no token issued twice");
    for view in views {
        assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
    }
}

// Registration: the early getter's table is the one published as kiln.eml.archive, and the capability rows are
// truthful: metadata and errors complete groups (with their member rows), read/codec/attributes unavailable.
#[test]
fn publication_registers_the_same_table_and_truthful_capabilities() {
    static ROWS: Mutex<Vec<(u64, String, i32, i32)>> = Mutex::new(Vec::new());
    unsafe extern "C" fn report(owner: u64, name: KilnStr, state: u32, reason: u32, _text: KilnStr) -> i32 {
        let name = unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) };
        ROWS.lock().unwrap().push((
            owner,
            String::from_utf8(name.to_vec()).unwrap(),
            state as i32,
            reason as i32,
        ));
        KILN_OK
    }
    let caps = KilnCapsApi {
        header: kiln_abi::KilnServiceHeader {
            size: std::mem::size_of::<KilnCapsApi>() as u32,
            version: 1,
        },
        count: None,
        at: None,
        find: None,
        report: Some(report),
        subscribe: None,
        unsubscribe: None,
        generation: None,
    };
    let mut registered = Vec::new();
    let rc = archive_service::publish(
        77,
        |name: &str, table: *const c_void| {
            registered.push((name.to_owned(), table));
            KILN_OK
        },
        Some(&caps),
    );
    assert_eq!(rc, KILN_OK);
    assert_eq!(
        registered,
        [("kiln.eml.archive".to_owned(), api() as *const _ as *const c_void)]
    );
    let rows: BTreeMap<String, (i32, i32)> = ROWS
        .lock()
        .unwrap()
        .iter()
        .map(|(o, n, s, r)| {
            assert_eq!(*o, 77);
            (n.clone(), (*s, *r))
        })
        .collect();
    let available = (KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE);
    for name in [
        "eml.archive.metadata",
        "eml.archive.metadata.acquire",
        "eml.archive.metadata.identity",
        "eml.archive.metadata.release",
        "eml.archive.metadata.registry_metadata",
        "eml.archive.errors",
        "eml.archive.errors.error_copy",
        "eml.archive.errors.error_release",
    ] {
        assert_eq!(rows.get(name), Some(&available), "{name}");
    }
    // The read group and its six member rows
    assert_eq!(rows.get("eml.archive.read"), Some(&available));
    // The attribute group and its three member rows
    assert_eq!(rows.get("eml.archive.attributes"), Some(&available));
    assert_eq!(rows.keys().filter(|n| n.starts_with("eml.archive.attributes.")).count(), 3);
    assert_eq!(rows.get("eml.archive.codec").map(|r| r.0), Some(KILN_CAP_UNAVAILABLE));
    assert!(!rows.keys().any(|n| n.starts_with("eml.archive.codec.")));
    assert_eq!(rows.keys().filter(|n| n.starts_with("eml.archive.read.")).count(), 6);
    assert_eq!(rows.len(), 20);
    // A refused registration reports no support.
    ROWS.lock().unwrap().clear();
    let rc = archive_service::publish(78, |_: &str, _: *const c_void| KILN_ERR_INVALID_ARGUMENT, Some(&caps));
    assert_eq!(rc, KILN_ERR_INVALID_ARGUMENT);
    assert!(ROWS.lock().unwrap().iter().all(|(_, _, s, _)| *s != KILN_CAP_AVAILABLE));
}

// A metadata read creates no game data write, baseline, lock or file at all: the provider holds no paths or handles.
#[test]
fn metadata_reads_touch_no_files() {
    let f = fixture::archive::ArchiveFixture::new("g10a-nofiles");
    let before = f.semantics();
    let listing = |root: &camino::Utf8Path| {
        let mut out = Vec::new();
        for e in walk(root.as_std_path()) {
            out.push((e.clone(), std::fs::read(&e).unwrap()));
        }
        out
    };
    let files = listing(&f.root.0);
    let (pins, _) = published();
    let view = acquire(next(), &pins).unwrap();
    page(view, 0, 9).unwrap();
    unsafe { api().release.unwrap()(view) };
    assert_eq!(listing(&f.root.0), files);
    assert_eq!(f.semantics(), before);
}

fn walk(dir: &std::path::Path) -> Vec<std::path::PathBuf> {
    let mut out = Vec::new();
    for e in std::fs::read_dir(dir).unwrap().flatten() {
        if e.file_type().unwrap().is_dir() {
            out.extend(walk(&e.path()));
        } else {
            out.push(e.path());
        }
    }
    out.sort();
    out
}

// The last reference frees a generation's registry snapshot: a closed view (released, revoked or retired) and a
// retired generation keep none; their tokens still answer as before.
#[test]
fn retired_generations_and_closed_views_free_their_snapshot() {
    let snap = snapshot();
    let weak = Arc::downgrade(&snap);
    let pins = archive_service::authorize(grant(Phase::Published, next()), snap).unwrap();
    let owner = next();
    let released = acquire(owner, &pins).unwrap();
    let revoked = acquire(owner, &pins).unwrap();
    let retired = acquire(next(), &pins).unwrap();
    assert_eq!(unsafe { api().release.unwrap()(released) }, KILN_OK);
    archive_service::revoke_owner(owner);
    assert!(weak.upgrade().is_some(), "an authorized generation keeps its snapshot");
    archive_service::retire(pins.generation);
    assert!(
        weak.upgrade().is_none(),
        "nothing keeps a retired generation's snapshot once its views are closed"
    );
    for view in [released, revoked, retired] {
        assert_eq!(page(view, 0, 0).unwrap_err().0, KILN_ERR_REVOKED);
        assert_eq!(
            unsafe { api().release.unwrap()(view) },
            KILN_OK,
            "release stays idempotent"
        );
    }
    assert_eq!(
        acquire(owner, &pins),
        Err(KILN_ERR_REVOKED),
        "a retired generation stays retired"
    );
}

// Every way a view closes reclaims its diagnostics: release (above), its owner's revocation and its generation's
// retirement. error_release stays idempotent afterwards.
#[test]
fn owner_revocation_and_retirement_reclaim_diagnostics() {
    let (pins, _) = published();
    let copy = api().error_copy.unwrap();
    let release = api().error_release.unwrap();
    let mut required = 0usize;
    let revoked = next();
    let view = acquire(revoked, &pins).unwrap();
    let (_, e) = page(view, reg::BROKEN as u64, 1).unwrap_err();
    assert_eq!(
        unsafe { copy(revoked, e.token, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_BUFFER_TOO_SMALL
    );
    archive_service::revoke_owner(revoked);
    assert_eq!(
        unsafe { copy(revoked, e.token, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_REVOKED
    );
    assert_eq!(unsafe { release(revoked, e.token) }, KILN_OK);
    let owner = next();
    let view = acquire(owner, &pins).unwrap();
    let (_, e) = page(view, reg::CYCLE_A as u64, 1).unwrap_err();
    assert_eq!(
        unsafe { copy(owner, e.token, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_BUFFER_TOO_SMALL
    );
    archive_service::retire(pins.generation);
    assert_eq!(
        unsafe { copy(owner, e.token, std::ptr::null_mut(), 0, &mut required) },
        KILN_ERR_REVOKED
    );
    assert_eq!(unsafe { release(owner, e.token) }, KILN_OK);
    assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
}

// A failing call that overlaps its view's release never leaves a diagnostic behind: the closing side reclaims the
// tokens it sees, the failing call one it issued after the view closed.
#[test]
fn a_diagnostic_issued_while_its_view_closes_is_reclaimed() {
    let (pins, _) = published();
    let copy = api().error_copy.unwrap();
    for round in 0..300 {
        let owner = next();
        let view = acquire(owner, &pins).unwrap();
        let failed = Arc::new(AtomicU64::new(0));
        let caller = {
            let failed = failed.clone();
            std::thread::spawn(move || {
                let mut tokens = Vec::new();
                loop {
                    match page(view, reg::BROKEN as u64, 1) {
                        Err((KILN_ERR_INTERNAL, f)) => {
                            tokens.push(f.token);
                            failed.fetch_add(1, Ordering::Relaxed);
                        }
                        Err((KILN_ERR_REVOKED, _)) => return tokens,
                        other => panic!("{other:?}"),
                    }
                }
            })
        };
        while failed.load(Ordering::Relaxed) < 1 + round % 7 {
            std::hint::spin_loop();
        }
        assert_eq!(unsafe { api().release.unwrap()(view) }, KILN_OK);
        let mut required = 0usize;
        for token in caller.join().unwrap() {
            assert_eq!(
                unsafe { copy(owner, token, std::ptr::null_mut(), 0, &mut required) },
                KILN_ERR_REVOKED,
                "round {round}: a closed view keeps no diagnostic"
            );
        }
    }
}

// registry_sha256 (Kiln's own definition): SHA-256 over every byte a page returns except its pin, so equal
// registries agree and any served difference (either version text, a type's metadata) changes it. registry_id is
// per snapshot.
#[test]
fn the_registry_digest_covers_every_byte_a_page_returns() {
    let (a, b) = (snapshot(), snapshot());
    assert_ne!(a.id(), b.id(), "registry ids are per snapshot");
    assert_eq!(a.sha256(), b.sha256(), "the same registry, the same digest");
    let mut changed = reg::types();
    changed[3].size += 1;
    let others = [
        ("game version", RegistrySnapshot::new(&reg::registry(), "m6-kfc-data-8")),
        (
            "registry version",
            RegistrySnapshot::new(&reg::registry_of("m6-other", reg::types()), "m6-kfc-data-7"),
        ),
        (
            "a type's metadata",
            RegistrySnapshot::new(&reg::registry_of(fixture::types::VERSION, changed), "m6-kfc-data-7"),
        ),
    ];
    for (what, other) in others {
        assert_ne!(other.sha256(), a.sha256(), "{what}");
    }
}
