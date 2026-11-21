// SPDX-License-Identifier: MIT
// the interpreter's loaded-resource field indexes.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/session/loaded_resource_indexes.rs (the whole state module: limits,
// jobs, lookups, rounds, invalidation, diagnostics), lua/values/validate_runtime_scalar.rs.
//! The loaded-resource field indexes of one VM: ShroudForge's shared, bounded, world-bound jobs that map one aligned
//! 32-bit scalar field of every loaded resource of a type to that resource, built over several rounds within a time
//! budget and confirmed by a fresh complete snapshot on every hit.
//!
//! - [`Indexes::register`] declares a job (one per type and field path, shared by every package that asks, at most
//!   [`MAX_INDEXES`] of them, at most [`MAX_OWNERS_PER_INDEX`] packages each); a declaration survives world changes.
//! - [`Indexes::advance`] is one round of the runtime's own runner ([`super::by_field::tick`], once per worker turn):
//!   a job lists its type once, then reads its scalar in batches of [`KEY_BATCH`] resources, at most
//!   [`MAX_ROUND_KEYS`] per round, until the round's [`ROUND_BUDGET`] is spent. A failed step drops the job's
//!   partial values and retries after [`RETRY_DELAY`].
//! - [`Indexes::lookup`] only inspects: a job still scanning answers its progress; a completed one its unique hit
//!   (read again in full and checked), "not found", or "ambiguous". A miss older than [`NEGATIVE_TTL`] queues the job
//!   again so resources loaded later are found.
//! - A world session change resets every job; a resource write resets the jobs whose field it overlaps.

use serde_json::{Value, json};
use std::collections::{HashMap, HashSet, VecDeque, hash_map::Entry};
use std::time::{Duration, Instant};

pub const MAX_INDEXES: usize = 16;
pub const MAX_RESOURCES: usize = 1 << 20;
pub const KEY_BATCH: usize = 64;
pub const MAX_ROUND_KEYS: usize = 1024;
pub const MAX_INDEX_BYTES: usize = 16 * 1024 * 1024;
/// Conservative admission estimate for the ID list, hash table and duplicate keys.
pub const RESOURCE_INDEX_BYTES: usize = 128;
pub const MAX_OWNERS_PER_INDEX: usize = 256;
pub const NEGATIVE_TTL: Duration = Duration::from_secs(2);
pub const RETRY_DELAY: Duration = Duration::from_millis(100);
pub const ROUND_BUDGET: Duration = Duration::from_millis(1);

/// ShroudForge's texts.
pub mod text {
    pub const OWNER_LIMIT: &str = "loaded resource index owner limit reached";
    pub const CAPACITY: &str = "loaded resource index capacity reached. Active jobs were preserved";
    pub const NOT_FOUND: &str = "loaded resource not found for indexed scalar";
    pub const AMBIGUOUS: &str = "loaded resource scalar is ambiguous in the completed index";
    pub const CANDIDATE_CHANGED: &str =
        "loaded resource index candidate changed. Index discarded, retry lookup";
    pub const MEMORY: &str = "loaded resource index exceeds the shared memory safety limit";
    pub const DUPLICATES: &str = "loaded resource listing contains duplicate ResourceIds";
    pub const UNEXPECTED_TYPE: &str =
        "loaded resource listing contains an unexpected resource type";
    pub const INCOMPLETE_BATCH: &str = "native scalar batch returned an incomplete result";
    /// `loaded resource index is still scanning ({scanned}/{total})`
    pub fn scanning(scanned: usize, total: usize) -> String {
        format!("loaded resource index is still scanning ({scanned}/{total})")
    }
}

/// A loaded resource's identity (ShroudForge's `ResourceId`: GUID, type hash, part).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ResourceId {
    pub guid: [u8; 16],
    pub type_hash: u32,
    pub part: u32,
}

impl ResourceId {
    pub fn type_hash(&self) -> u32 {
        self.type_hash
    }
}

/// One job's identity: the registry type and the field path.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Selector {
    pub root: usize,
    pub path: String,
}

/// Where a job's scalar sits: the resource type, its reflected root size, the field's offset and its scalar kind
/// (the registry's primitive type name after typedefs).
#[derive(Clone, Copy, Debug)]
pub struct Projection {
    pub type_hash: u32,
    pub root_size: usize,
    pub offset: usize,
    pub kind: &'static str,
}

pub struct Index {
    pub(crate) selector: Selector,
    pub(crate) projection: Projection,
    pub(crate) owners: HashSet<String>,
    pub(crate) resources: Vec<ResourceId>,
    pub(crate) listed: bool,
    pub(crate) scanned: usize,
    /// None denotes two or more different resources with the same scalar bits.
    pub(crate) values: HashMap<[u8; 4], Option<ResourceId>>,
    pub(crate) completed_at: Option<Instant>,
    pub(crate) retry_at: Instant,
    pub(crate) failure: Option<String>,
}

impl Index {
    fn reset(&mut self, now: Instant, reason: Option<String>) {
        self.resources = Vec::new();
        self.values = HashMap::new();
        self.listed = false;
        self.scanned = 0;
        self.completed_at = None;
        self.retry_at = if reason.is_some() {
            now + RETRY_DELAY
        } else {
            now
        };
        self.failure = reason;
    }

    fn estimated_bytes(&self) -> usize {
        self.resources.len().saturating_mul(RESOURCE_INDEX_BYTES)
    }
}

#[derive(Default)]
struct Metrics {
    rounds: u64,
    batches: u64,
    scalar_reads: u64,
    payload_reads: u64,
    failures: u64,
    resets: u64,
    budget_overruns: u64,
    last_round_us: u64,
    maximum_round_us: u64,
}

#[derive(Default)]
pub struct Indexes {
    pub(crate) session: Option<u64>,
    pub(crate) entries: VecDeque<Index>,
    metrics: Metrics,
}

/// A job's progress (the members' `progress` table).
#[derive(Debug)]
pub struct Progress {
    pub session_id: u64,
    pub scanned: usize,
    pub total: usize,
    pub payload_reads: usize,
    pub complete: bool,
    pub phase: &'static str,
}

/// A lookup's answer: the confirmed resource and its snapshot, or the reason, and the progress.
#[derive(Debug)]
pub struct Lookup {
    pub resource: Option<(ResourceId, Vec<u8>)>,
    pub reason: Option<String>,
    pub progress: Progress,
}

/// ShroudForge's `validate_runtime_scalar` (validate_runtime_scalar.rs:4-19).
pub fn validate_scalar(kind: &str, bytes: &[u8]) -> Result<(), String> {
    crate::runtime::ecs::cas::validate_runtime_scalar(kind, bytes)
}

impl Indexes {
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
    pub fn len(&self) -> usize {
        self.entries.len()
    }
    pub fn pending_reason(&self, position: usize) -> Option<String> {
        self.entries[position].failure.clone()
    }

    /// Keep declarations so a confirmed world change or mutation restarts work
    /// without requiring the mod to perform its gameplay action again.
    pub fn invalidate(&mut self) {
        let now = Instant::now();
        for index in &mut self.entries {
            index.reset(now, None);
            self.metrics.resets += 1;
        }
    }

    pub fn invalidate_resource(&mut self, type_hash: u32, offset: usize, width: usize) {
        let Some(end) = offset.checked_add(width) else {
            self.invalidate();
            return;
        };
        let now = Instant::now();
        for index in &mut self.entries {
            let key = index.projection.offset;
            if index.projection.type_hash == type_hash && offset < key + 4 && key < end {
                index.reset(now, None);
                self.metrics.resets += 1;
            }
        }
    }

    pub fn remove_owner(&mut self, owner: &str) {
        self.entries.retain_mut(|index| {
            index.owners.remove(owner);
            !index.owners.is_empty()
        });
    }

    pub fn observe_session(&mut self, session: u64, now: Instant) {
        if self.session != Some(session) {
            for index in &mut self.entries {
                index.reset(now, None);
                self.metrics.resets += 1;
            }
            self.session = Some(session);
        }
    }

    pub fn register(
        &mut self,
        session: u64,
        owner: &str,
        selector: Selector,
        projection: Projection,
        now: Instant,
    ) -> Result<usize, String> {
        self.observe_session(session, now);
        if let Some(position) = self
            .entries
            .iter()
            .position(|index| index.selector == selector)
        {
            let index = &mut self.entries[position];
            if !index.owners.contains(owner) && index.owners.len() >= MAX_OWNERS_PER_INDEX {
                return Err(text::OWNER_LIMIT.into());
            }
            index.owners.insert(owner.to_owned());
            return Ok(position);
        }
        if self.entries.len() >= MAX_INDEXES {
            return Err(text::CAPACITY.into());
        }
        let position = self.entries.len();
        self.entries.push_back(Index {
            selector,
            projection,
            owners: HashSet::from([owner.to_owned()]),
            resources: Vec::new(),
            listed: false,
            scanned: 0,
            values: HashMap::new(),
            completed_at: None,
            retry_at: now,
            failure: None,
        });
        Ok(position)
    }

    pub fn progress(&self, position: usize) -> Progress {
        let index = &self.entries[position];
        Progress {
            session_id: self.session.unwrap_or_default(),
            scanned: index.scanned,
            total: index.resources.len(),
            payload_reads: 0,
            complete: index.completed_at.is_some(),
            phase: if index.completed_at.is_some() {
                "complete"
            } else {
                "indexing"
            },
        }
    }

    /// Queries register and inspect work. Only the runner advances discovery.
    pub fn lookup(
        &mut self,
        position: usize,
        wanted: [u8; 4],
        now: Instant,
        mut read: impl FnMut(&ResourceId) -> Result<Vec<u8>, String>,
        mut key: impl FnMut(&[u8]) -> Result<[u8; 4], String>,
    ) -> Result<Lookup, String> {
        let index = &mut self.entries[position];
        if index.completed_at.is_some_and(|completed| {
            index.values.get(&wanted).copied().flatten().is_none()
                && now.saturating_duration_since(completed) >= NEGATIVE_TTL
        }) {
            index.reset(now, None);
            self.metrics.resets += 1;
        }
        let mut result = Lookup {
            resource: None,
            reason: None,
            progress: self.progress(position),
        };
        let index = &mut self.entries[position];
        if index.completed_at.is_none() {
            result.reason = Some(
                index
                    .failure
                    .clone()
                    .unwrap_or_else(|| text::scanning(index.scanned, index.resources.len())),
            );
            return Ok(result);
        }
        match index.values.get(&wanted).copied() {
            None => result.reason = Some(text::NOT_FOUND.into()),
            Some(None) => result.reason = Some(text::AMBIGUOUS.into()),
            Some(Some(id)) => {
                self.metrics.payload_reads += 1;
                let confirmed = read(&id).and_then(|bytes| {
                    if key(&bytes)? != wanted {
                        return Err(text::CANDIDATE_CHANGED.into());
                    }
                    Ok(bytes)
                });
                match confirmed {
                    Ok(bytes) => {
                        result.progress.payload_reads = 1;
                        result.resource = Some((id, bytes));
                    }
                    Err(reason) => {
                        index.reset(now, Some(reason.clone()));
                        self.metrics.failures += 1;
                        self.metrics.resets += 1;
                        return Err(reason);
                    }
                }
            }
        }
        Ok(result)
    }

    pub fn advance(
        &mut self,
        session: u64,
        now: Instant,
        mut list: impl FnMut(u32) -> Result<Vec<ResourceId>, String>,
        mut batch: impl FnMut(&[ResourceId], Projection) -> Result<Vec<[u8; 4]>, String>,
    ) {
        self.observe_session(session, now);
        let started = Instant::now();
        let deadline = started + ROUND_BUDGET;
        let mut round_keys = 0;
        let mut idle = 0;
        while !self.entries.is_empty() && Instant::now() < deadline && round_keys < MAX_ROUND_KEYS {
            let mut index = self.entries.pop_front().expect("not empty");
            if index.completed_at.is_some() || now < index.retry_at {
                self.entries.push_back(index);
                idle += 1;
                if idle >= self.entries.len() {
                    break;
                }
                continue;
            }
            idle = 0;
            let others_bytes: usize = self.entries.iter().map(Index::estimated_bytes).sum();
            let work = (|| -> Result<(), String> {
                if !index.listed {
                    let resources = list(index.projection.type_hash)?;
                    if resources.len() > MAX_RESOURCES
                        || others_bytes
                            .saturating_add(resources.len().saturating_mul(RESOURCE_INDEX_BYTES))
                            > MAX_INDEX_BYTES
                    {
                        return Err(text::MEMORY.into());
                    }
                    let unique: HashSet<_> = resources.iter().copied().collect();
                    if unique.len() != resources.len() {
                        return Err(text::DUPLICATES.into());
                    }
                    if resources
                        .iter()
                        .any(|id| id.type_hash() != index.projection.type_hash)
                    {
                        return Err(text::UNEXPECTED_TYPE.into());
                    }
                    index.resources = resources;
                    index.listed = true;
                }
                // The listing itself consumes this round's cooperative budget.
                if Instant::now() >= deadline {
                    return Ok(());
                }
                let end = (index.scanned + KEY_BATCH).min(index.resources.len());
                if end > index.scanned {
                    let ids = &index.resources[index.scanned..end];
                    self.metrics.batches += 1;
                    let keys = batch(ids, index.projection)?;
                    if keys.len() != ids.len() {
                        return Err(text::INCOMPLETE_BATCH.into());
                    }
                    for key in &keys {
                        validate_scalar(index.projection.kind, key)?;
                    }
                    for (id, key) in ids.iter().copied().zip(keys) {
                        match index.values.entry(key) {
                            Entry::Vacant(entry) => {
                                entry.insert(Some(id));
                            }
                            Entry::Occupied(mut entry) => {
                                entry.insert(None);
                            }
                        }
                    }
                    let count = end - index.scanned;
                    index.scanned = end;
                    round_keys += count;
                    self.metrics.scalar_reads += count as u64;
                }
                if index.scanned == index.resources.len() {
                    index.completed_at = Some(now);
                    index.failure = None;
                }
                Ok(())
            })();
            if let Err(reason) = work {
                index.reset(now, Some(reason));
                self.metrics.failures += 1;
                self.metrics.resets += 1;
            }
            self.entries.push_back(index);
        }
        let elapsed = started.elapsed();
        self.metrics.rounds += 1;
        self.metrics.last_round_us = elapsed.as_micros().min(u128::from(u64::MAX)) as u64;
        self.metrics.maximum_round_us = self
            .metrics
            .maximum_round_us
            .max(self.metrics.last_round_us);
        self.metrics.budget_overruns += u64::from(elapsed > ROUND_BUDGET);
    }

    /// ShroudForge's `resourceIndexes` status record (publish_runtime_status.rs:8).
    pub fn diagnostics(&self) -> Value {
        let ready = self
            .entries
            .iter()
            .filter(|index| index.completed_at.is_some())
            .count();
        json!({"jobs":self.entries.len(),"queued":self.entries.len()-ready,"ready":ready,
            "estimatedBytes":self.entries.iter().map(Index::estimated_bytes).sum::<usize>(),
            "maximumBytes":MAX_INDEX_BYTES,"roundBudgetMicroseconds":ROUND_BUDGET.as_micros() as u64,
            "rounds":self.metrics.rounds,"scalarBatches":self.metrics.batches,
            "scalarReads":self.metrics.scalar_reads,"candidatePayloadReads":self.metrics.payload_reads,
            "failures":self.metrics.failures,"resets":self.metrics.resets,
            "lastRoundMicroseconds":self.metrics.last_round_us,"maximumRoundMicroseconds":self.metrics.maximum_round_us,
            "budgetOverruns":self.metrics.budget_overruns})
    }
}

#[cfg(test)]
#[path = "index_tests.rs"]
mod tests;
