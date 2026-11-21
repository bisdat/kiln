// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (identical in both pins): modules/mod-runtime/lua/ingame/overrides/
// claim_runtime_storage.rs:1-3 (no baseline, a leaked active token keeps its claim), 15-121
// (Target, its session rule and labels, OwnershipStore), 123-139 (check_write, component_target).
//! The worker-local ownership store.
//!
//! One store per runtime-create, shared by every package (ShroudForge's
//! `AppState.runtime_ownership`). A record says which mod owns one whole storage (a component
//! on an entity, a loaded resource, a named service) and which of that mod's tokens hold it.
//! The store keeps no baseline and no pointer. Another mod's acquire, and its checked writers
//! (`check_write`), are refused while a record exists; the same mod may hold one storage with
//! several tokens, and the record goes when the last of them is released. A token whose claim
//! is dropped while active keeps the record (releasing it silently would let another mod
//! capture a changed value).
//!
//! Component and named-service keys carry the world session they were made in; a resource's
//! key does not, because loaded resources can outlive a world. [`OwnershipStore::prune`]
//! with a nonzero session drops every session-bound record of another session; session 0 (no
//! world, or a core that cannot say) is no evidence that a world ended and prunes nothing.

use sfi_core::bindings::runtime::RuntimeOperation;
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};

/// The reason when the store has handed out its last token.
pub const TOKEN_EXHAUSTED: &str = "runtime claim token space exhausted";

/// A loaded resource's identity (ShroudForge's `ResourceId`: GUID, type hash, part index),
/// independent of the world session. `guid` is the GUID as ShroudForge prints it (its label
/// shows only the GUID); the resource family renders it.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct ResourceKey {
    pub guid: String,
    pub type_hash: u32,
    pub part_index: u32,
}

/// The identity of one claimed storage (ShroudForge's `Target`).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum StorageKey {
    /// A component on an entity, in the world session it was claimed in.
    Component {
        session: u64,
        entity: u32,
        qualified_name: String,
    },
    /// A loaded resource (and every template on it): no session, it can survive a world
    /// change; the session belongs to the claim token, never to this identity.
    Resource(ResourceKey),
    /// A named service, in the world session it was claimed in.
    Service { session: u64, key: String },
}

impl StorageKey {
    /// Whether a record of this storage survives into `current_session`.
    pub fn belongs_to(&self, current_session: u64) -> bool {
        match self {
            Self::Component { session, .. } | Self::Service { session, .. } => {
                *session == current_session
            }
            Self::Resource(_) => true,
        }
    }

    /// The storage's name in the ownership refusal.
    pub fn label(&self) -> String {
        match self {
            Self::Component {
                entity,
                qualified_name,
                ..
            } => format!("component {qualified_name} on entity {entity}"),
            Self::Resource(resource) => format!("loaded resource {}", resource.guid),
            Self::Service { key, .. } => format!("named service {key}"),
        }
    }

    /// The operations an acquire of this storage checks, in ShroudForge's order.
    pub fn operations(&self) -> &'static [RuntimeOperation] {
        match self {
            Self::Resource(_) => &[RuntimeOperation::EcsWrite, RuntimeOperation::AssetsWrite],
            _ => &[RuntimeOperation::EcsWrite],
        }
    }
}

#[derive(Debug)]
struct Record {
    owner: String,
    tokens: HashSet<u64>,
}

/// The claims of one runtime-create, on the worker thread.
#[derive(Debug, Default)]
pub struct OwnershipStore {
    next_token: Cell<u64>,
    records: RefCell<HashMap<StorageKey, Record>>,
}

impl OwnershipStore {
    pub fn new() -> Self {
        Self::default()
    }

    /// A store whose next token follows `last`: for tests of token exhaustion.
    #[doc(hidden)]
    pub fn with_last_token(last: u64) -> Self {
        let store = Self::default();
        store.next_token.set(last);
        store
    }

    /// A fresh token, never handed out before by this store.
    pub fn token(&self) -> Result<u64, String> {
        let next = self
            .next_token
            .get()
            .checked_add(1)
            .ok_or(TOKEN_EXHAUSTED)?;
        self.next_token.set(next);
        Ok(next)
    }

    /// Drops the records of session-bound storage of another session. Session 0 keeps all.
    pub fn prune(&self, session: u64) {
        if session == 0 {
            return;
        }
        self.records
            .borrow_mut()
            .retain(|key, _| key.belongs_to(session));
    }

    /// Refuses `owner` when another mod owns `key`.
    pub fn check(&self, owner: &str, key: &StorageKey) -> Result<(), String> {
        if let Some(record) = self.records.borrow().get(key)
            && record.owner != owner
        {
            return Err(format!(
                "{} is owned by runtime mod '{}'. Disable its setting and restore it before another mod writes this storage",
                key.label(),
                record.owner
            ));
        }
        Ok(())
    }

    /// Records `token` of `owner` on `key`, unless another mod owns it.
    pub fn acquire(&self, owner: &str, key: &StorageKey, token: u64) -> Result<(), String> {
        self.check(owner, key)?;
        self.records
            .borrow_mut()
            .entry(key.clone())
            .or_insert_with(|| Record {
                owner: owner.into(),
                tokens: HashSet::new(),
            })
            .tokens
            .insert(token);
        Ok(())
    }

    /// Takes `token` of `owner` off `key`; the record goes with its last token. Another
    /// mod's release changes nothing.
    pub fn release(&self, owner: &str, key: &StorageKey, token: u64) {
        let mut records = self.records.borrow_mut();
        let Some(record) = records.get_mut(key) else {
            return;
        };
        if record.owner != owner {
            return;
        }
        record.tokens.remove(&token);
        if record.tokens.is_empty() {
            records.remove(key);
        }
    }

    /// ShroudForge's `check_write`: a typed writer's check right before its mutation. Prunes
    /// to the live session first; unclaimed storage may be written, another mod's claim
    /// refuses (also after that mod stopped).
    pub fn check_write(
        &self,
        owner: &str,
        key: &StorageKey,
        live_session: u64,
    ) -> Result<(), String> {
        self.prune(live_session);
        self.check(owner, key)
    }

    /// Who owns `key` now, if anyone.
    pub fn owner_of(&self, key: &StorageKey) -> Option<String> {
        self.records.borrow().get(key).map(|r| r.owner.clone())
    }

    /// How many tokens hold `key` now.
    pub fn tokens_on(&self, key: &StorageKey) -> usize {
        self.records.borrow().get(key).map_or(0, |r| r.tokens.len())
    }

    /// How many storages are claimed now.
    pub fn len(&self) -> usize {
        self.records.borrow().len()
    }

    pub fn is_empty(&self) -> bool {
        self.records.borrow().is_empty()
    }
}
