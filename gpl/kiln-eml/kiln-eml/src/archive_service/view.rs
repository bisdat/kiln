// SPDX-License-Identifier: GPL-3.0-or-later
//! Authorized generations and the views issued over them: acquire, identity, release and registry_metadata
//!, plus the startup controller side (`authorize`, `retire`, `retire_epoch`, `revoke_owner`).
//!
//! Only a generation the startup controller authorized can be acquired, and only with every pin equal (no paths, no
//! wildcards). A view binds its owner and that generation's pins and registry snapshot; a newer generation never
//! retargets it. A call clones the generation's `Arc` under the lock and works on it afterwards, so a release,
//! revocation or retirement meanwhile stops new calls without freeing what a call in flight reads. No lock is held
//! while a call builds or copies its reply. View tokens are process-unique, nonzero and never reused; a closed view
//! stays known as a tombstone (its owner and generation number only), so release stays idempotent and later uses read
//! REVOKED. Closed views and retired generations drop their reference: the last view or call that still reads a
//! generation frees its registry snapshot.
//!
//! A generation may also keep the kfc registry its snapshot was built from (`authorize_with_registry`, the C table): the
//! source kiln.eml.values' `arena_create` copies once per arena. Only the snapshot ever answers here.
//!
//! A generation authorized with its archive (`authorize_with_archive`, the archive table's read group) also serves the read group
//! (`super::read`): it keeps the registry, the `ArchiveBacking` and the resource_json replies materialized for it.
use std::{
    collections::{BTreeMap, HashMap},
    ffi::c_char,
    sync::{Arc, Mutex, MutexGuard},
};

use kfc::reflection::TypeRegistry;
use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_REVOKED, KILN_ERR_UNAVAILABLE,
    KILN_OK, KilnOwner, KilnResult,
    services::kiln_eml_archive::{
        KILN_EML_ARCHIVE_PREPARED, KILN_EML_ARCHIVE_PUBLISHED, KilnArchiveErrorV1, KilnEmlArchiveIdentityV1,
        KilnEmlArchiveView,
    },
};

use super::{
    errors::{self, ErrorOut, overlap},
    read::{ArchiveBacking, Reply},
    registry::{self, RegistrySnapshot, u64s},
};
use kfc::guid::ResourceId;

const IDENTITY_SIZE: u32 = std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;

/// A generation's phase (the identity record's `phase`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    /// Staged, not yet live; only owners the startup controller names may acquire it.
    Prepared,
    /// Live; any owner may acquire a runtime grant.
    Published,
}

/// What the startup controller authorizes: one immutable generation's pins. The registry pins come from the
/// snapshot it is authorized with, never from the caller.
#[derive(Debug, Clone)]
pub struct Grant {
    pub phase: Phase,
    /// RFC 4122 byte order.
    pub launch_id: [u8; 16],
    pub plan_id: [u8; 16],
    pub generation: u64,
    pub reload_epoch: u64,
    pub plan_sha256: [u8; 32],
    pub archive_sha256: [u8; 32],
    /// For a PREPARED generation: the owners allowed to acquire it.
    pub prepared_owners: Vec<KilnOwner>,
}

pub(super) struct Generation {
    pub(super) pins: KilnEmlArchiveIdentityV1,
    snapshot: Arc<RegistrySnapshot>,
    /// The kfc registry the startup controller built `snapshot` from, when it gave one (kiln.eml.values and the read group
    /// need it).
    pub(super) registry: Option<Arc<TypeRegistry>>,
    prepared_owners: Vec<KilnOwner>,
    /// The serialized `pin` object every page carries.
    pub(super) pin_json: String,
    /// The archive the read group reads; None: the read members answer UNAVAILABLE.
    pub(super) archive: Option<Arc<ArchiveBacking>>,
    /// resource_json's replies, materialized once per resource for this generation and kept with it: a size probe and
    /// the copy after it read the same bytes, and no reply is built twice from a file that may have changed.
    pub(super) replies: Mutex<HashMap<ResourceId, Arc<Reply>>>,
}

impl Generation {
    /// The materialized reply for `id`, built by `build` the first time. No lock is held while it builds; when two
    /// first calls race, the first reply stored is the one every call answers with.
    pub(super) fn reply(&self, id: ResourceId, build: impl FnOnce() -> Reply) -> Arc<Reply> {
        if let Some(reply) = self.replies_lock().get(&id) {
            return reply.clone();
        }
        let built = Arc::new(build());
        self.replies_lock().entry(id).or_insert(built).clone()
    }

    fn replies_lock(&self) -> MutexGuard<'_, HashMap<ResourceId, Arc<Reply>>> {
        self.replies.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

struct GenerationEntry {
    pins: KilnEmlArchiveIdentityV1,
    /// None once retired: the number stays taken and its pins answer REVOKED.
    open: Option<Arc<Generation>>,
}

struct View {
    owner: KilnOwner,
    generation: u64,
    /// What the view reads while it is open; None once closed (a tombstone).
    open: Option<Arc<Generation>>,
}

struct State {
    next_view: u64,
    generations: BTreeMap<u64, GenerationEntry>,
    views: BTreeMap<u64, View>,
}

static STATE: Mutex<State> = Mutex::new(State {
    next_view: 1,
    generations: BTreeMap::new(),
    views: BTreeMap::new(),
});

fn state() -> MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn same_pins(a: &KilnEmlArchiveIdentityV1, b: &KilnEmlArchiveIdentityV1) -> bool {
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

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn uuid(b: &[u8; 16]) -> String {
    format!(
        "{}-{}-{}-{}-{}",
        hex(&b[..4]),
        hex(&b[4..6]),
        hex(&b[6..8]),
        hex(&b[8..10]),
        hex(&b[10..])
    )
}

/// Authorizes one generation over `snapshot` and returns the pins a consumer acquires it with. Refused for a zero
/// generation and for a generation number already authorized (with any pins). A generation authorized this way keeps
/// no kfc registry, so kiln.eml.values cannot create an arena over it (UNAVAILABLE).
pub fn authorize(grant: Grant, snapshot: Arc<RegistrySnapshot>) -> Result<KilnEmlArchiveIdentityV1, String> {
    authorize_generation(grant, snapshot, None, None)
}

/// `authorize`, keeping beside the snapshot the kfc registry it was built from: kiln.eml.values' `arena_create` copies
/// it once per arena and checks that copy's digest against the view's `registry_sha256` before any value exists. A
/// registry that is not the snapshot's source is not refused here; every arena over it is (UNAVAILABLE).
pub fn authorize_with_registry(
    grant: Grant,
    snapshot: Arc<RegistrySnapshot>,
    registry: Arc<TypeRegistry>,
) -> Result<KilnEmlArchiveIdentityV1, String> {
    authorize_generation(grant, snapshot, Some(registry), None)
}

/// `authorize_with_registry` plus the generation's archive: the read group (resource_json, find, list and
/// read resources, content ranges and lists) answers from `archive`, decoding with `registry`. Unlike
/// `authorize_with_registry`, a registry that is not the snapshot's source is refused here (its digest differs), and so
/// is an archive whose reference index names another game version than the snapshot: every read reply carries this
/// generation's pins, so its registry and archive must be the ones they name.
pub fn authorize_with_archive(
    grant: Grant,
    snapshot: Arc<RegistrySnapshot>,
    registry: Arc<TypeRegistry>,
    archive: Arc<ArchiveBacking>,
) -> Result<KilnEmlArchiveIdentityV1, String> {
    if registry::digest(&registry, snapshot.game_version()) != *snapshot.sha256() {
        return Err("the registry is not the one the snapshot was built from".into());
    }
    if archive.game_version() != snapshot.game_version() {
        return Err(format!(
            "the archive's reference index is game version {:?}, the snapshot's {:?}",
            archive.game_version(),
            snapshot.game_version()
        ));
    }
    authorize_generation(grant, snapshot, Some(registry), Some(archive))
}

fn authorize_generation(
    grant: Grant,
    snapshot: Arc<RegistrySnapshot>,
    registry: Option<Arc<TypeRegistry>>,
    archive: Option<Arc<ArchiveBacking>>,
) -> Result<KilnEmlArchiveIdentityV1, String> {
    if grant.generation == 0 {
        return Err("generation 0 is not a generation".into());
    }
    let pins = KilnEmlArchiveIdentityV1 {
        size: IDENTITY_SIZE,
        phase: match grant.phase {
            Phase::Prepared => KILN_EML_ARCHIVE_PREPARED as u32,
            Phase::Published => KILN_EML_ARCHIVE_PUBLISHED as u32,
        },
        launch_id: grant.launch_id,
        plan_id: grant.plan_id,
        generation: grant.generation,
        reload_epoch: grant.reload_epoch,
        plan_sha256: grant.plan_sha256,
        archive_sha256: grant.archive_sha256,
        registry_id: snapshot.id(),
        registry_sha256: *snapshot.sha256(),
    };
    let pin_json = format!(
        concat!(
            "{{\"launch_id\":\"{}\",\"plan_id\":\"{}\",\"generation\":\"{}\",\"reload_epoch\":\"{}\",",
            "\"plan_sha256\":\"{}\",\"archive_sha256\":\"{}\",\"registry_id\":\"{}\",\"registry_sha256\":\"{}\"}}"
        ),
        uuid(&pins.launch_id),
        uuid(&pins.plan_id),
        u64s(pins.generation),
        u64s(pins.reload_epoch),
        hex(&pins.plan_sha256),
        hex(&pins.archive_sha256),
        u64s(pins.registry_id),
        hex(&pins.registry_sha256),
    );
    let mut state = state();
    if state.generations.contains_key(&grant.generation) {
        return Err(format!("generation {} is already authorized", grant.generation));
    }
    let generation = Arc::new(Generation {
        pins,
        snapshot,
        registry,
        prepared_owners: grant.prepared_owners,
        pin_json,
        archive,
        replies: Mutex::new(HashMap::new()),
    });
    state.generations.insert(
        pins.generation,
        GenerationEntry {
            pins,
            open: Some(generation),
        },
    );
    Ok(pins)
}

/// Closes the open views `select` picks: returns their tokens, and moves their references into `dropped` so the
/// caller frees them after the lock.
fn close_views(
    state: &mut State,
    mut select: impl FnMut(&View) -> bool,
    dropped: &mut Vec<Arc<Generation>>,
) -> Vec<u64> {
    let mut closed = Vec::new();
    for (token, view) in state.views.iter_mut() {
        if view.open.is_some() && select(view) {
            dropped.extend(view.open.take());
            closed.push(*token);
        }
    }
    closed
}

/// Retires a generation: its views are revoked and it cannot be acquired again. Unknown or already retired
/// generations are ignored.
pub fn retire(generation: u64) {
    let mut dropped = Vec::new();
    let closed = {
        let mut state = state();
        let Some(entry) = state.generations.get_mut(&generation) else {
            return;
        };
        let Some(open) = entry.open.take() else {
            return;
        };
        dropped.push(open);
        close_views(&mut state, |view| view.generation == generation, &mut dropped)
    };
    errors::reclaim_views(&closed);
}

/// Retires every generation of a reload epoch.
pub fn retire_epoch(reload_epoch: u64) {
    let generations: Vec<u64> = state()
        .generations
        .values()
        .filter(|e| e.open.is_some() && e.pins.reload_epoch == reload_epoch)
        .map(|e| e.pins.generation)
        .collect();
    for generation in generations {
        retire(generation);
    }
}

/// An owner's revocation closes all its views (and their diagnostics).
pub fn revoke_owner(owner: KilnOwner) {
    let mut dropped = Vec::new();
    let closed = close_views(&mut state(), |view| view.owner == owner, &mut dropped);
    errors::reclaim_views(&closed);
}

/// The live view's owner and generation, or the result for an unknown (INVALID_ARGUMENT) or closed (REVOKED) one.
pub(super) fn live(token: KilnEmlArchiveView) -> Result<(KilnOwner, Arc<Generation>), KilnResult> {
    let state = state();
    match state.views.get(&token) {
        None => Err(KILN_ERR_INVALID_ARGUMENT),
        Some(View { open: None, .. }) => Err(KILN_ERR_REVOKED),
        Some(View {
            owner,
            open: Some(generation),
            ..
        }) => Ok((*owner, generation.clone())),
    }
}

/// A live view's pins (kiln.eml.values' `value_open` owner view): INVALID_ARGUMENT for an unknown view, REVOKED for a
/// closed one.
pub(crate) fn view_pins(view: KilnEmlArchiveView) -> Result<KilnEmlArchiveIdentityV1, KilnResult> {
    live(view).map(|(_, generation)| generation.pins)
}

/// What kiln.eml.values' `arena_create` resolves a live view to.
pub(crate) struct RegistrySource {
    /// The generation's pins: `registry_id` and `registry_sha256` are its snapshot's.
    pub(crate) pins: KilnEmlArchiveIdentityV1,
    /// The game version the snapshot was built with (part of its digest).
    pub(crate) game_version: String,
    /// The kfc registry kept beside it; None for a generation authorized without one.
    pub(crate) registry: Option<Arc<TypeRegistry>>,
    /// The generation's archive, which RESOURCE roots read; None for a generation authorized without one.
    pub(crate) archive: Option<Arc<ArchiveBacking>>,
}

/// A live view's registry source: INVALID_ARGUMENT for an unknown view, REVOKED for a closed one. The generation's
/// reference is cloned under the lock; nothing is copied or digested while it is held.
pub(crate) fn registry_source(view: KilnEmlArchiveView) -> Result<RegistrySource, KilnResult> {
    let (_, generation) = live(view)?;
    Ok(RegistrySource {
        pins: generation.pins,
        game_version: generation.snapshot.game_version().to_owned(),
        registry: generation.registry.clone(),
        archive: generation.archive.clone(),
    })
}

pub(super) unsafe extern "C" fn acquire(
    owner: KilnOwner,
    expected: *const KilnEmlArchiveIdentityV1,
    out: *mut KilnEmlArchiveView,
) -> KilnResult {
    super::contained(|| {
        if owner == 0 || expected.is_null() || out.is_null() {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let size = unsafe { std::ptr::addr_of!((*expected).size).read_unaligned() };
        if size < IDENTITY_SIZE
            || overlap(&[
                (expected as usize, size as usize),
                (out as usize, std::mem::size_of::<u64>()),
            ])
        {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let want = unsafe { expected.read_unaligned() };
        let phase_known =
            want.phase == KILN_EML_ARCHIVE_PREPARED as u32 || want.phase == KILN_EML_ARCHIVE_PUBLISHED as u32;
        if !phase_known || want.generation == 0 || want.registry_id == 0 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let mut state = state();
        let Some(entry) = state.generations.get(&want.generation) else {
            return KILN_ERR_UNAVAILABLE;
        };
        if !same_pins(&entry.pins, &want) {
            return KILN_ERR_UNAVAILABLE;
        }
        let Some(generation) = entry.open.clone() else {
            return KILN_ERR_REVOKED;
        };
        if want.phase == KILN_EML_ARCHIVE_PREPARED as u32 && !generation.prepared_owners.contains(&owner) {
            return KILN_ERR_UNAVAILABLE;
        }
        let token = state.next_view;
        state.next_view += 1;
        state.views.insert(
            token,
            View {
                owner,
                generation: want.generation,
                open: Some(generation),
            },
        );
        unsafe { out.write_unaligned(token) };
        KILN_OK
    })
}

pub(super) unsafe extern "C" fn identity(view: KilnEmlArchiveView, out: *mut KilnEmlArchiveIdentityV1) -> KilnResult {
    super::contained(|| {
        if out.is_null() {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let size = unsafe { std::ptr::addr_of!((*out).size).read_unaligned() };
        if size < IDENTITY_SIZE {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let (_, generation) = match live(view) {
            Ok(found) => found,
            Err(rc) => return rc,
        };
        let mut copy = generation.pins;
        copy.size = size;
        unsafe { out.write_unaligned(copy) };
        KILN_OK
    })
}

pub(super) unsafe extern "C" fn release(view: KilnEmlArchiveView) -> KilnResult {
    super::contained(|| {
        // Freed after the lock (unless a call in flight still reads it).
        let _dropped = {
            let mut state = state();
            let Some(entry) = state.views.get_mut(&view) else {
                return KILN_ERR_INVALID_ARGUMENT;
            };
            entry.open.take()
        };
        errors::reclaim_views(&[view]);
        KILN_OK
    })
}

pub(super) unsafe extern "C" fn registry_metadata(
    view: KilnEmlArchiveView,
    first: u64,
    limit: u32,
    json: *mut c_char,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    // Checked before any lookup: an invalid record is never written.
    let Ok(error) = (unsafe { ErrorOut::check(error) }) else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    let outputs = [
        (json as usize, capacity),
        (required as usize, std::mem::size_of::<usize>()),
    ];
    // A record that overlaps another output is not valid and is not written; a valid one is initialized on every
    // return, this refusal included.
    if outputs.iter().any(|&span| overlap(&[span, error.span()])) {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    if overlap(&outputs) {
        return error.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
    }
    let work = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if required.is_null() || (json.is_null() && capacity != 0) {
            return error.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let (owner, generation) = match live(view) {
            Ok(found) => found,
            Err(rc) => return error.finish(rc, 0, 0),
        };
        let snapshot = &generation.snapshot;
        let count = snapshot.len();
        if first > count || first.checked_add(u64::from(limit)).is_none() {
            return error.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let end = if limit == 0 {
            first
        } else {
            (first + u64::from(limit)).min(count)
        };
        let reply = match snapshot.page(&generation.pin_json, first, end) {
            Ok(reply) => reply,
            Err(cause) => {
                let rc = error.fail(KILN_ERR_INTERNAL, owner, view, cause);
                // A release, revocation or retirement meanwhile closes the view first and then reclaims the
                // diagnostics it finds; a token this call issued after that is reclaimed here, so a closed view never
                // keeps one.
                if live(view).is_err() {
                    errors::reclaim_views(&[view]);
                }
                return rc;
            }
        };
        unsafe { required.write_unaligned(reply.len()) };
        if capacity < reply.len() {
            return error.finish(KILN_ERR_BUFFER_TOO_SMALL, 0, 0);
        }
        unsafe { std::ptr::copy_nonoverlapping(reply.as_ptr(), json.cast::<u8>(), reply.len()) };
        error.finish(KILN_OK, 0, 0)
    }));
    // A contained panic is INTERNAL; the record is initialized all the same (no retained cause).
    work.unwrap_or_else(|_| error.finish(KILN_ERR_INTERNAL, 0, 0))
}
