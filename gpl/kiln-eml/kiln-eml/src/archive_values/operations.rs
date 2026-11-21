// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.values v1's operations over the typed graph, in Rust terms: the C table wraps each one in its C member
//! (record size checks, KilnStr and buffers) and adds nothing to its semantics.
//!
//! # Replies
//! A worker operation returns a [`Reply`] (the header's fixed `KilnEmlValueResultV1`) or a [`Refusal`] (its KilnResult):
//!
//! | Operation | item | payload | arity | MUTATED |
//! |---|---|---|---|---|
//! | value_open | the root node (a scalar root: a scalar node) | VIEW of a scalar root | 1 | no |
//! | value_child | a typed child; 0 for a scalar (its VIEW is the payload) | VIEW of a scalar child | 1 | no |
//! | value_set | 0 | 0 | 0 | yes |
//! | value_clone | the clone (a scalar node for a scalar) | VIEW of a scalar | 1 | no |
//! | value_export | 0 | the serialized bytes | 1 | no |
//! | value_read VIEW, LENGTH, DIRTY, STRINGIFY | 0 | the packet | 1 | no |
//! | value_read SOURCE_BYTES | 0 | the bytes; 0 for Source's None | 1 | no |
//! | value_read ITER_BEGIN | the iterator | 0 | 3 | no |
//! | value_read ITER_NEXT (iterator lease) | a typed value; else 0 | ITER_NEXT | 2 | no |
//! | push, insert, clear | 0 | 0 | 1 | yes |
//! | pop, remove that removed | the removed node (a nil node for a removed nil) | VIEW of a scalar | 1 | yes |
//! | pop, remove that removed nothing | 0 | 0 | 1 | no |
//! | an array operation on a live non-array value | 0 | 0 | 0 (UNSUPPORTED) | no |
//! | a Source error | 0 | 0 | 0 (SEMANTIC_ERROR, `error` lease) | no |
//!
//! Every lease a reply returns is issued before the operation's own edit, and Source never errors after its edit, so a
//! SEMANTIC_ERROR leaves the receiver as a plain read would (a mapped child cached by the read may stay).
//! `receiver_revision` is the receiver's edit count after the call. A wrong-kind token, a FIELD selector on an array, an
//! INDEX selector on a struct or variant, LENGTH/STRINGIFY/ITER_BEGIN on a variant, scalar, iterator or packet, and
//! ITER_NEXT on anything but an iterator are INVALID_ARGUMENT (no reader call, no effect).
//!
//! # Borrows
//! As Source: `__index`, `#`, `__tostring`, `pairs` (ITER_BEGIN), clone, dirtiness and export borrow the receiver
//! shared; `__newindex` on a struct or array borrows it shared and a variant's `value` setter mutably; push, insert,
//! pop, remove and clear borrow it mutably for the whole call. A conflict is Source's borrow error (SEMANTIC_ERROR). An
//! iterator's steps (ITER_NEXT) borrow nothing: Source's iterator userdata holds the value's cache, not the value's
//! userdata, so a step works even while an array operation on that value runs (`*ValueIter::step` in owned_values.rs
//! and mapped_values.rs).
//!
//! # Proxies
//! Where Source keeps a child's userdata in a cache slot (struct fields, array items, a variant's value, mapped read
//! caches, a RESOURCE root's shared ResourceInfo), the first read that hands the child out asks the host for that
//! userdata (MAKE_TYPED_PROXY with a provisional lease the new proxy adopts) and pins it in the slot. Fresh roots,
//! LOADED_SNAPSHOT roots (their frontend ResourceInfo keeps them), clones and removed values are never pinned by the
//! graph: the caller's reference keeps them.
use std::{panic::AssertUnwindSafe, rc::Rc};

use kfc::{
    reflection::{LookupKey, TypeIndex},
    resource::value::Value,
};
use kiln_abi::{
    KilnOwner,
    services::{
        kiln_assets::KilnAssetKey,
        kiln_eml_archive::KilnEmlArchiveIdentityV1,
        kiln_eml_values::{
            KILN_VALUE_DIRTY, KILN_VALUE_ERROR, KILN_VALUE_HANDLED, KILN_VALUE_INFO_DIRTY, KILN_VALUE_INFO_LENGTH,
            KILN_VALUE_INFO_SOURCE_LENGTH, KILN_VALUE_ITERATOR, KILN_VALUE_LOADED_SNAPSHOT, KILN_VALUE_MUTATED,
            KILN_VALUE_OPEN_BYTES, KILN_VALUE_OPEN_DEFAULT, KILN_VALUE_OPEN_LOADED_SNAPSHOT, KILN_VALUE_OPEN_LUA_INPUT,
            KILN_VALUE_OPEN_RESOURCE, KILN_VALUE_READ_DIRTY, KILN_VALUE_READ_ITER_BEGIN, KILN_VALUE_READ_ITER_NEXT,
            KILN_VALUE_READ_LENGTH, KILN_VALUE_READ_SOURCE_BYTES, KILN_VALUE_READ_STRINGIFY, KILN_VALUE_READ_VIEW,
            KILN_VALUE_RESOURCE_ROOT, KILN_VALUE_RESULT, KILN_VALUE_SEMANTIC_ERROR, KILN_VALUE_STARTUP,
            KILN_VALUE_STATIC_ARRAY, KILN_VALUE_UNSUPPORTED, KilnEmlValueIdentityV1, KilnEmlValueInfoV1,
            KilnEmlValueResultV1,
        },
    },
};

use super::{
    arena::{ArenaLocal, Held, RootKey, enter_control, enter_lease},
    input::{Src, Validator, describe, host_error, transport},
    lifetime::{Access, Fault, LeaseDisposition, MutBorrow, Pin, Refusal, SharedBorrow},
    node::{Body, Converter, Iter, Node, Slot, SlotRef, Val, clone_node, is_dirty, lazily, mapped_to_val, node_text},
    packet,
};
use crate::sf_startup::resource_values::{TypeHandle, decode_mapped};

/// The fixed reply of a worker operation (`KilnEmlValueResultV1` without its size).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Reply {
    pub disposition: u32,
    pub flags: u32,
    pub arity: u32,
    pub item: u64,
    pub payload: u64,
    pub error: u64,
    pub receiver_revision: u64,
}

impl Reply {
    fn handled(arity: u32, revision: u64) -> Self {
        Self {
            disposition: KILN_VALUE_HANDLED as u32,
            arity,
            receiver_revision: revision,
            ..Self::default()
        }
    }
    fn mutated(arity: u32, revision: u64) -> Self {
        Self {
            flags: KILN_VALUE_MUTATED as u32,
            ..Self::handled(arity, revision)
        }
    }
    pub fn is_mutated(&self) -> bool {
        self.flags & KILN_VALUE_MUTATED as u32 != 0
    }
    /// The header's record.
    pub fn record(&self) -> KilnEmlValueResultV1 {
        KilnEmlValueResultV1 {
            size: std::mem::size_of::<KilnEmlValueResultV1>() as u32,
            disposition: self.disposition,
            flags: self.flags,
            arity: self.arity,
            item: self.item,
            payload: self.payload,
            error: self.error,
            receiver_revision: self.receiver_revision,
        }
    }
}

/// A `KilnEmlValueSelectorV1`: a struct or variant FIELD, or a 1-based array INDEX (the frontend already did Source's
/// key conversion).
#[derive(Debug, Clone, Copy)]
pub enum Selector<'a> {
    Field(&'a [u8]),
    Index(u64),
}

/// A `KilnEmlValueOpenV1` (bytes are copied).
#[derive(Clone, Copy)]
pub struct OpenRequest<'a> {
    pub origin: u32,
    pub qualified_type: &'a [u8],
    pub key: KilnAssetKey,
    pub source_instance: u64,
    pub source_session: u64,
    pub bytes: &'a [u8],
    pub lua_object: u64,
}

/// A panic never leaves an operation: it is INTERNAL.
fn contained<R>(work: impl FnOnce() -> Result<R, Refusal>) -> Result<R, Refusal> {
    std::panic::catch_unwind(AssertUnwindSafe(work)).unwrap_or(Err(Refusal::Internal))
}

/// A Source error as the reply: the error lease is its packet (and the host errors it keeps). If even that lease
/// cannot be issued, the operation is refused.
fn failed(local: &ArenaLocal, fault: Fault, revision: u64) -> Result<Reply, Refusal> {
    let (bytes, pins) = packet::error(fault);
    let error = local.issue_packet(bytes, true, pins)?;
    Ok(Reply {
        disposition: KILN_VALUE_SEMANTIC_ERROR as u32,
        error,
        receiver_revision: revision,
        ..Reply::default()
    })
}

/// Runs a step whose Source error becomes the reply.
macro_rules! step {
    ($local:expr, $revision:expr, $work:expr) => {
        match $work {
            Ok(value) => value,
            Err(fault) => return failed($local, fault, $revision),
        }
    };
}

fn value_node(held: Held) -> Result<Rc<Node>, Refusal> {
    match held {
        Held::Value(node) => Ok(node),
        _ => Err(Refusal::InvalidArgument),
    }
}

fn type_named(local: &ArenaLocal, name: &[u8]) -> Result<TypeHandle, Refusal> {
    let name = std::str::from_utf8(name).map_err(|_| Refusal::InvalidArgument)?;
    let found = local
        .registry
        .get_by_name(LookupKey::Qualified(name))
        .ok_or(Refusal::InvalidArgument)?;
    Ok(TypeHandle::new(local.registry.clone(), found.index))
}

fn same_identity(a: &KilnEmlArchiveIdentityV1, b: &KilnEmlArchiveIdentityV1) -> bool {
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

// ---- proxies ----

/// MAKE_TYPED_PROXY for `node`: the temporary object token of its userdata. A new proxy adopts a provisional lease (its
/// finalizer closes it once); a reused proxy or a failure leaves the provisional lease to GPL, which withdraws it.
pub(crate) fn make_proxy(local: &ArenaLocal, node: &Rc<Node>) -> Result<u64, Fault> {
    let provisional = local
        .issue_node(node.clone(), None)
        .map_err(|refusal| Fault::internal(format!("no provisional lease for a proxy ({refusal:?})")))?;
    let reply = local.host.access(&Access::MakeTypedProxy {
        provisional,
        arena: local.id(),
        node_id: node.id,
    });
    let reply = match reply {
        Ok(reply) => reply,
        Err(refused) => {
            local.withdraw(provisional);
            return Err(transport("MAKE_TYPED_PROXY", refused));
        }
    };
    if reply.error_object != 0 {
        local.withdraw(provisional);
        return Err(host_error(local, reply.error_object));
    }
    if reply.lease != LeaseDisposition::Adopted {
        local.withdraw(provisional);
    }
    if reply.object == 0 {
        return Err(Fault::internal("MAKE_TYPED_PROXY returned no object"));
    }
    Ok(reply.object)
}

/// Pins `child`'s userdata into `parent`'s slot at `at`, where Source's slot keeps its `LuaValue`.
fn pin_child(local: &ArenaLocal, parent: &Node, at: &SlotRef, child: &Rc<Node>) -> Result<(), Fault> {
    if parent.slot_pinned(at, child) {
        return Ok(());
    }
    let object = make_proxy(local, child)?;
    let pin = Pin::retain(&*local.host, &local.sink, object).map_err(|refused| transport("retain", refused))?;
    // a callback replaced the slot meanwhile: the pin drops (the result lease still keeps the child)
    let _ = parent.pin_slot(at, child, pin);
    Ok(())
}

/// Pins a cached root's userdata into its root record (Source's ResourceInfo value cache).
fn pin_root(local: &ArenaLocal, key: RootKey, node: &Rc<Node>) -> Result<(), Fault> {
    let pinned = local.roots.borrow().get(&key).is_some_and(|slot| slot.pin.is_some());
    if pinned {
        return Ok(());
    }
    let object = make_proxy(local, node)?;
    let pin = Pin::retain(&*local.host, &local.sink, object).map_err(|refused| transport("retain", refused))?;
    let mut roots = local.roots.borrow_mut();
    if let Some(slot) = roots.get_mut(&key)
        && matches!(&slot.val, Val::Node(n) if Rc::ptr_eq(n, node))
        && slot.pin.is_none()
    {
        slot.pin = Some(pin);
    }
    Ok(())
}

/// Source's `to_string` of a typed userdata without `__tostring` (a variant): its frontend userdata's text.
pub(crate) fn proxy_text(local: &ArenaLocal, node: &Rc<Node>) -> Result<Vec<u8>, Fault> {
    let object = make_proxy(local, node)?;
    let reply = local
        .host
        .access(&Access::SourceToString { object })
        .map_err(|refused| transport("SOURCE_TO_STRING", refused))?;
    if reply.error_object != 0 {
        return Err(host_error(local, reply.error_object));
    }
    local
        .host
        .copy(reply.object)
        .map_err(|refused| transport("copy", refused))
}

// ---- replies that hand out a value ----

/// A value as an item: a typed node is its lease; a scalar is a scalar node's lease plus its VIEW.
fn value_reply(local: &ArenaLocal, val: Val, arity: u32, revision: u64) -> Result<Reply, Refusal> {
    match val {
        Val::Node(node) if node.scalar_val().is_none() => {
            let item = local.issue_node(node, None)?;
            Ok(Reply {
                item,
                ..Reply::handled(arity, revision)
            })
        }
        scalar => {
            let scalar = match scalar {
                Val::Node(node) => node.scalar_val().cloned().unwrap_or(Val::Nil),
                other => other,
            };
            let payload = local.issue_packet(packet::view_scalar(&scalar), false, Vec::new())?;
            let item = match local.issue_node(Node::scalar(local, scalar), None) {
                Ok(item) => item,
                Err(refusal) => {
                    local.withdraw(payload);
                    return Err(refusal);
                }
            };
            Ok(Reply {
                item,
                payload,
                ..Reply::handled(arity, revision)
            })
        }
    }
}

// ---- open ----

/// `value_open`: a root of the arena's phase. RESOURCE resolves the shared ResourceInfo root by key (cached, its
/// userdata pinned by the record); LOADED_SNAPSHOT the root of one loaded ResourceInfo instance (recorded weakly with no
/// pin: the frontend keeps that root's userdata on its ResourceInfo, so the same instance answers the same node while
/// it lives and a fresh decode once it is gone); BYTES, DEFAULT and LUA_INPUT make fresh roots. No origin enrolls
/// anything in a stage or writes anything.
pub fn value_open(
    owner: KilnOwner,
    arena: u64,
    view: &KilnEmlArchiveIdentityV1,
    request: &OpenRequest<'_>,
) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope) = enter_control(arena, owner)?;
        if !same_identity(view, &local.pins.archive) {
            return Err(Refusal::InvalidArgument);
        }
        let decode = |bytes: &[u8], r#type: &TypeHandle| -> Result<Val, Fault> {
            let mapped = decode_mapped(Rc::from(bytes), r#type).map_err(Fault::external)?;
            mapped_to_val(&mapped, &*local)
        };
        match request.origin as i32 {
            KILN_VALUE_OPEN_RESOURCE => {
                let source = local.resources.clone().ok_or(Refusal::Unavailable)?;
                let key = RootKey::Resource {
                    guid: request.key.guid,
                    r#type: request.key.r#type,
                    part: request.key.part,
                };
                open_root(&local, key, KILN_VALUE_RESOURCE_ROOT as u32, || {
                    match source.read(&request.key).map_err(Fault::external)? {
                        None => Ok(Val::Nil),
                        Some((bytes, index)) => {
                            let r#type = TypeHandle::try_new(local.registry.clone(), TypeIndex::new(index))
                                .ok_or_else(|| Fault::external("resource type not found in current game registry"))?;
                            decode(&bytes, &r#type)
                        }
                    }
                })
            }
            KILN_VALUE_OPEN_LOADED_SNAPSHOT => {
                let r#type = type_named(&local, request.qualified_type)?;
                let key = RootKey::Loaded {
                    instance: request.source_instance,
                    session: request.source_session,
                };
                // weak, unpinned: the frontend's ResourceInfo keeps the root's userdata, as Source's value cache does
                if let Some(node) = local.loaded_root(&key) {
                    let revision = node.revision.get();
                    return value_reply(&local, Val::Node(node), 1, revision);
                }
                let val = step!(&local, 0, decode(request.bytes, &r#type));
                if let Val::Node(node) = &val {
                    node.flags.set(node.flags.get() | KILN_VALUE_LOADED_SNAPSHOT as u32);
                    local.keep_loaded(key, node);
                }
                value_reply(&local, val, 1, 0)
            }
            KILN_VALUE_OPEN_BYTES => {
                let r#type = type_named(&local, request.qualified_type)?;
                let val = step!(&local, 0, decode(request.bytes, &r#type));
                value_reply(&local, val, 1, 0)
            }
            KILN_VALUE_OPEN_DEFAULT => {
                let r#type = type_named(&local, request.qualified_type)?;
                let bytes = r#type.default_value.clone().ok_or(Refusal::Unavailable)?;
                let val = step!(&local, 0, decode(&bytes, &r#type));
                value_reply(&local, val, 1, 0)
            }
            KILN_VALUE_OPEN_LUA_INPUT => {
                let r#type = type_named(&local, request.qualified_type)?;
                let src = step!(&local, 0, describe(&local, request.lua_object));
                let val = step!(&local, 0, Validator::validate(&local, &src, &r#type));
                value_reply(&local, val, 1, 0)
            }
            _ => Err(Refusal::InvalidArgument),
        }
    })
}

fn open_root(
    local: &ArenaLocal,
    key: RootKey,
    flag: u32,
    load: impl FnOnce() -> Result<Val, Fault>,
) -> Result<Reply, Refusal> {
    let cached = local.roots.borrow().get(&key).map(|slot| slot.val.clone());
    let val = match cached {
        Some(val) => val,
        None => {
            // Source caches the root (nil included) only once its read succeeded
            let val = step!(local, 0, load());
            if let Val::Node(node) = &val {
                node.flags.set(node.flags.get() | flag);
            }
            local.roots.borrow_mut().insert(key, Slot::new(val.clone()));
            val
        }
    };
    let revision = match &val {
        Val::Node(node) => {
            step!(local, node.revision.get(), pin_root(local, key, node));
            node.revision.get()
        }
        _ => 0,
    };
    value_reply(local, val, 1, revision)
}

// ---- info ----

/// `value_info`: identity and class always (no borrow, allowed re-entrantly, even under MAKE_TYPED_PROXY); lengths and
/// dirtiness only as requested. A dirtiness walk Source would refuse (a conflicting borrow, too deep) is UNAVAILABLE.
pub fn value_info(owner: KilnOwner, value: u64, requested: u32) -> Result<KilnEmlValueInfoV1, Refusal> {
    contained(|| {
        let known = (KILN_VALUE_INFO_LENGTH | KILN_VALUE_INFO_DIRTY | KILN_VALUE_INFO_SOURCE_LENGTH) as u32;
        if requested & !known != 0 {
            return Err(Refusal::InvalidArgument);
        }
        let (local, _scope, held) = enter_lease(value, owner)?;
        let identity = |node_id: u64, revision: u64, creator: KilnOwner| KilnEmlValueIdentityV1 {
            size: std::mem::size_of::<KilnEmlValueIdentityV1>() as u32,
            lua_phase: local.phase,
            creator_owner: creator,
            arena: local.id(),
            node_id,
            revision,
            layout_revision: local.pins.layout_revision,
            archive: local.pins.archive,
        };
        let mut info = KilnEmlValueInfoV1 {
            size: std::mem::size_of::<KilnEmlValueInfoV1>() as u32,
            requested,
            identity: identity(0, 0, local.shared.host_owner),
            kind: 0,
            flags: 0,
            type_index: 0,
            qualified_hash: 0,
            logical_len: 0,
            source_len: 0,
        };
        match held {
            Held::Iterator(_) => info.kind = KILN_VALUE_ITERATOR as u32,
            Held::Packet(packet) => {
                info.kind = if packet.error {
                    KILN_VALUE_ERROR as u32
                } else {
                    KILN_VALUE_RESULT as u32
                }
            }
            Held::Value(node) => {
                info.identity = identity(node.id, node.revision.get(), node.creator);
                info.kind = node.kind();
                info.flags = node.flags.get();
                if node.is_static_array() {
                    info.flags |= KILN_VALUE_STATIC_ARRAY as u32;
                }
                if let Some((_, r#type)) = node.classify() {
                    info.type_index = r#type.index.as_usize() as u32;
                    info.qualified_hash = r#type.qualified_hash;
                }
                if requested & KILN_VALUE_INFO_LENGTH as u32 != 0 {
                    info.logical_len = match &node.body {
                        Body::Struct { fields, .. } => fields.borrow().len() as u64,
                        Body::MStruct { data, .. } => {
                            lazily(|| Ok(data.len())).map_err(|_| Refusal::Unavailable)? as u64
                        }
                        _ => node.logical_len().unwrap_or(0) as u64,
                    };
                }
                if requested & KILN_VALUE_INFO_SOURCE_LENGTH as u32 != 0 {
                    info.source_len = node.source_len().unwrap_or(0) as u64;
                }
                if requested & KILN_VALUE_INFO_DIRTY as u32 != 0
                    && is_dirty(&Val::Node(node.clone()), 0).map_err(|_| Refusal::Unavailable)?
                {
                    info.flags |= KILN_VALUE_DIRTY as u32;
                }
            }
        }
        Ok(info)
    })
}

// ---- read ----

fn is_struct_or_array(node: &Node) -> bool {
    matches!(
        node.body,
        Body::Struct { .. } | Body::MStruct { .. } | Body::Array { .. } | Body::MArray { .. }
    )
}

/// `value_read`: VIEW, LENGTH, DIRTY, STRINGIFY, SOURCE_BYTES, ITER_BEGIN on a value lease; VIEW and ITER_NEXT on an
/// iterator lease.
pub fn value_read(owner: KilnOwner, value: u64, query: u32) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(value, owner)?;
        match (query as i32, held) {
            (KILN_VALUE_READ_VIEW, Held::Value(node)) => {
                let payload = local.issue_packet(packet::view(&node), false, Vec::new())?;
                Ok(Reply {
                    payload,
                    ..Reply::handled(1, node.revision.get())
                })
            }
            (KILN_VALUE_READ_VIEW, Held::Iterator(iter)) => {
                let payload = local.issue_packet(packet::view_iterator(), false, Vec::new())?;
                Ok(Reply {
                    payload,
                    ..Reply::handled(1, iter.node.revision.get())
                })
            }
            (KILN_VALUE_READ_LENGTH, Held::Value(node)) if is_struct_or_array(&node) => {
                let revision = node.revision.get();
                let _borrow = step!(&local, revision, node.borrow.shared());
                let len = step!(&local, revision, node.lua_len()).ok_or(Refusal::InvalidArgument)?;
                let payload = local.issue_packet(packet::length(len), false, Vec::new())?;
                Ok(Reply {
                    payload,
                    ..Reply::handled(1, revision)
                })
            }
            (KILN_VALUE_READ_DIRTY, Held::Value(node)) => {
                let revision = node.revision.get();
                let dirty = step!(&local, revision, is_dirty(&Val::Node(node.clone()), 0));
                let payload = local.issue_packet(packet::dirty(dirty), false, Vec::new())?;
                Ok(Reply {
                    payload,
                    ..Reply::handled(1, revision)
                })
            }
            (KILN_VALUE_READ_STRINGIFY, Held::Value(node)) if is_struct_or_array(&node) => {
                let revision = node.revision.get();
                let text = step!(&local, revision, node_text(&node, &*local, 0)).ok_or(Refusal::InvalidArgument)?;
                let payload = local.issue_packet(text, false, Vec::new())?;
                Ok(Reply {
                    payload,
                    ..Reply::handled(1, revision)
                })
            }
            (KILN_VALUE_READ_SOURCE_BYTES, Held::Value(node)) => {
                let revision = node.revision.get();
                let bytes = match &node.body {
                    Body::MStruct { .. } => {
                        let _borrow = step!(&local, revision, node.borrow.shared());
                        node.source_bytes()
                    }
                    _ => None,
                };
                let payload = match bytes {
                    Some(bytes) => local.issue_packet(bytes, false, Vec::new())?,
                    None => 0,
                };
                Ok(Reply {
                    payload,
                    ..Reply::handled(1, revision)
                })
            }
            (KILN_VALUE_READ_ITER_BEGIN, Held::Value(node)) if is_struct_or_array(&node) => {
                let revision = node.revision.get();
                let iter = {
                    let _borrow = step!(&local, revision, node.borrow.shared());
                    step!(&local, revision, Iter::begin(&node)).ok_or(Refusal::InvalidArgument)?
                };
                let item = local.issue_iterator(iter)?;
                Ok(Reply {
                    item,
                    ..Reply::handled(3, revision)
                })
            }
            (KILN_VALUE_READ_ITER_NEXT, Held::Iterator(iter)) => iter_next(&local, &iter),
            _ => Err(Refusal::InvalidArgument),
        }
    })
}

fn iter_next(local: &ArenaLocal, iter: &Iter) -> Result<Reply, Refusal> {
    let node = iter.node.clone();
    let revision = node.revision.get();
    // no Source borrow of the node: SF's iterators step over the cache they share with it
    let (key, value, at) = step!(local, revision, iter.step(local));
    let mut item = 0;
    if let (Val::Node(child), Some(at)) = (&value, &at)
        && child.scalar_val().is_none()
    {
        step!(local, revision, pin_child(local, &node, at, child));
        item = local.issue_node(child.clone(), None)?;
    }
    let payload = match local.issue_packet(packet::iter_next(&key, &value), false, Vec::new()) {
        Ok(payload) => payload,
        Err(refusal) => {
            local.withdraw(item);
            return Err(refusal);
        }
    };
    Ok(Reply {
        item,
        payload,
        ..Reply::handled(2, node.revision.get())
    })
}

// ---- child, set ----

fn slot_of(node: &Node, selector: &Selector<'_>, setting: bool) -> Result<SlotRef, Refusal> {
    let field = |key: &[u8]| -> Result<String, Refusal> {
        std::str::from_utf8(key)
            .map(str::to_owned)
            .map_err(|_| Refusal::InvalidArgument)
    };
    match (&node.body, selector) {
        (Body::Struct { .. } | Body::MStruct { .. }, Selector::Field(key)) => Ok(SlotRef::Field(field(key)?)),
        // a variant has only the fields `type` and `value`, and only `value` has a setter
        (Body::Variant { .. } | Body::MVariant { .. }, Selector::Field(key))
            if *key == b"value" || (*key == b"type" && !setting) =>
        {
            Ok(SlotRef::Field(field(key)?))
        }
        (Body::Array { .. } | Body::MArray { .. }, Selector::Index(index)) => Ok(SlotRef::Index(
            usize::try_from(*index).map_err(|_| Refusal::InvalidArgument)?,
        )),
        _ => Err(Refusal::InvalidArgument),
    }
}

/// `value_child`: Source's `__index` (a variant's `type`/`value` getters), caching what Source caches.
pub fn value_child(owner: KilnOwner, value: u64, selector: &Selector<'_>) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(value, owner)?;
        let node = value_node(held)?;
        let at = slot_of(&node, selector, false)?;
        let revision = node.revision.get();
        let _borrow = step!(&local, revision, node.borrow.shared());
        let val = step!(&local, revision, node.get(&*local, &at)).ok_or(Refusal::InvalidArgument)?;
        match &val {
            Val::Node(child) if child.scalar_val().is_none() => {
                step!(&local, revision, pin_child(&local, &node, &at, child));
                let item = local.issue_node(child.clone(), None)?;
                Ok(Reply {
                    item,
                    ..Reply::handled(1, node.revision.get())
                })
            }
            scalar => {
                let payload = local.issue_packet(packet::view_scalar(scalar), false, Vec::new())?;
                Ok(Reply {
                    payload,
                    ..Reply::handled(1, node.revision.get())
                })
            }
        }
    })
}

/// A held Source borrow of either mode.
#[allow(dead_code)]
enum Guard<'a> {
    Shared(SharedBorrow<'a>),
    Mut(MutBorrow<'a>),
}

/// `value_set`: Source's `__newindex` (a variant's `value` setter): Source's own checks, then validation of the value
/// against the field's or element's type (the fork's fix), then the edit.
pub fn value_set(owner: KilnOwner, value: u64, selector: &Selector<'_>, lua_object: u64) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(value, owner)?;
        let node = value_node(held)?;
        let at = slot_of(&node, selector, true)?;
        let revision = node.revision.get();
        let _guard = match &node.body {
            Body::Variant { .. } | Body::MVariant { .. } => {
                Guard::Mut(step!(&local, revision, node.borrow.exclusive()))
            }
            _ => Guard::Shared(step!(&local, revision, node.borrow.shared())),
        };
        let target = step!(&local, revision, node.set_target(&at)).ok_or(Refusal::InvalidArgument)?;
        let src = step!(&local, revision, describe(&local, lua_object));
        let val = step!(&local, node.revision.get(), Validator::validate(&local, &src, &target));
        if local.closed() {
            return Err(Refusal::Revoked);
        }
        step!(&local, node.revision.get(), node.set_commit(&at, val));
        node.revision.set(node.revision.get() + 1);
        Ok(Reply::mutated(0, node.revision.get()))
    })
}

// ---- clone, export ----

/// `value_clone`: SF `try_clone_lua_value` (shared borrows); a mapped clone keeps its original bytes and dirtiness.
pub fn value_clone(owner: KilnOwner, value: u64) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(value, owner)?;
        let node = value_node(held)?;
        let revision = node.revision.get();
        let clone = step!(&local, revision, clone_node(&node, &*local, 0));
        value_reply(&local, Val::Node(clone), 1, revision)
    })
}

/// `value_export`: the value as resource bytes of its own exact type, or (`selected_type` nonempty) validated and cloned
/// against that type first, as SF's `encode_owned_value` does. The bytes are retained: a size probe never encodes twice.
pub fn value_export(owner: KilnOwner, value: u64, selected_type: &[u8]) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(value, owner)?;
        let node = value_node(held)?;
        let revision = node.revision.get();
        let (val, r#type) = if selected_type.is_empty() {
            let r#type = node
                .classify()
                .map(|(_, t)| t.clone())
                .ok_or(Refusal::InvalidArgument)?;
            (Val::Node(node.clone()), r#type)
        } else {
            let r#type = type_named(&local, selected_type)?;
            let checked = step!(
                &local,
                revision,
                Validator::validate(&local, &Src::Local(Val::Node(node.clone())), &r#type)
            );
            (checked, r#type)
        };
        let converted = step!(&local, revision, Converter::convert(&val, &r#type, &*local));
        let bytes = step!(&local, revision, serialize(&local, &converted, &r#type));
        let payload = local.issue_packet(bytes, false, Vec::new())?;
        Ok(Reply {
            payload,
            ..Reply::handled(1, revision)
        })
    })
}

fn serialize(local: &ArenaLocal, value: &Value, r#type: &TypeHandle) -> Result<Vec<u8>, Fault> {
    std::panic::catch_unwind(AssertUnwindSafe(|| value.to_bytes(&local.registry, r#type)))
        .map_err(|panic| Fault::external(format!("serializing failed: {}", super::node::panic_text(panic))))?
        .map_err(|e| Fault::external(e.to_string()))
}

// ---- result copy ----

/// `value_result_copy`: a payload or error packet's bytes, copied into `out` when it is large enough; returns the size
/// needed either way. Never re-runs anything.
pub fn value_result_copy(owner: KilnOwner, result: u64, out: &mut [u8]) -> Result<usize, Refusal> {
    contained(|| {
        let (_local, _scope, held) = enter_lease(result, owner)?;
        let Held::Packet(packet) = held else {
            return Err(Refusal::InvalidArgument);
        };
        let required = packet.bytes.len();
        if out.len() >= required {
            out[..required].copy_from_slice(&packet.bytes);
        }
        Ok(required)
    })
}

// ---- array operations ----

/// The array an array operation works on: an iterator or packet is INVALID_ARGUMENT, any other live value that is no
/// array is answered UNSUPPORTED.
fn array_of(held: Held) -> Result<Result<Rc<Node>, Reply>, Refusal> {
    let node = value_node(held)?;
    if node.is_array() {
        Ok(Ok(node))
    } else {
        Ok(Err(Reply {
            disposition: KILN_VALUE_UNSUPPORTED as u32,
            receiver_revision: node.revision.get(),
            ..Reply::default()
        }))
    }
}

macro_rules! array_or_unsupported {
    ($held:expr) => {
        match array_of($held)? {
            Ok(node) => node,
            Err(reply) => return Ok(reply),
        }
    };
}

/// `table.insert(array, value)`: insert at len + 1.
pub fn value_array_push(owner: KilnOwner, array: u64, lua_object: u64) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(array, owner)?;
        let node = array_or_unsupported!(held);
        let _borrow = step!(&local, node.revision.get(), node.borrow.exclusive());
        let len = node.logical_len().unwrap_or(0);
        insert_at(&local, &node, len + 1, lua_object)
    })
}

/// `table.insert(array, index, value)`.
pub fn value_array_insert(owner: KilnOwner, array: u64, index: u64, lua_object: u64) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(array, owner)?;
        let node = array_or_unsupported!(held);
        let index = usize::try_from(index).map_err(|_| Refusal::InvalidArgument)?;
        let _borrow = step!(&local, node.revision.get(), node.borrow.exclusive());
        insert_at(&local, &node, index, lua_object)
    })
}

fn insert_at(local: &ArenaLocal, node: &Node, index: usize, lua_object: u64) -> Result<Reply, Refusal> {
    let revision = node.revision.get();
    let target = step!(local, revision, node.insert_target(index));
    let src = step!(local, revision, describe(local, lua_object));
    let val = step!(local, revision, Validator::validate(local, &src, &target));
    if local.closed() {
        return Err(Refusal::Revoked);
    }
    step!(local, revision, node.insert_commit(index, val));
    node.revision.set(revision + 1);
    Ok(Reply::mutated(1, revision + 1))
}

/// `table.remove(array)`: remove at len.
pub fn value_array_pop(owner: KilnOwner, array: u64) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(array, owner)?;
        let node = array_or_unsupported!(held);
        let _borrow = step!(&local, node.revision.get(), node.borrow.exclusive());
        let len = node.logical_len().unwrap_or(0);
        remove_at(&local, &node, len)
    })
}

/// `table.remove(array, index)`.
pub fn value_array_remove(owner: KilnOwner, array: u64, index: u64) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(array, owner)?;
        let node = array_or_unsupported!(held);
        let index = usize::try_from(index).map_err(|_| Refusal::InvalidArgument)?;
        let _borrow = step!(&local, node.revision.get(), node.borrow.exclusive());
        remove_at(&local, &node, index)
    })
}

fn remove_at(local: &ArenaLocal, node: &Node, index: usize) -> Result<Reply, Refusal> {
    let revision = node.revision.get();
    let Some(val) = step!(local, revision, node.remove_prepare(index, local)) else {
        // position 0, len + 1, an empty pop: nothing removed, nothing changed
        return Ok(Reply::handled(1, revision));
    };
    // the replies first: a refusal now leaves the array as it was
    let (item_node, scalar) = match &val {
        Val::Node(child) => (child.clone(), None),
        other => (Node::scalar(local, other.clone()), Some(other.clone())),
    };
    let item = local.issue_node(item_node, None)?;
    let payload = match &scalar {
        Some(scalar) => match local.issue_packet(packet::view_scalar(scalar), false, Vec::new()) {
            Ok(payload) => payload,
            Err(refusal) => {
                local.withdraw(item);
                return Err(refusal);
            }
        },
        None => 0,
    };
    if local.closed() {
        local.withdraw(item);
        local.withdraw(payload);
        return Err(Refusal::Revoked);
    }
    let slot = match node.remove_commit(index, &val) {
        Ok(slot) => slot,
        Err(fault) => {
            local.withdraw(item);
            local.withdraw(payload);
            return failed(local, fault, revision);
        }
    };
    if let Some(pin) = slot.pin {
        // the removed value's userdata stays until the caller has it
        local.attach_pin(item, pin);
    }
    node.revision.set(revision + 1);
    Ok(Reply {
        item,
        payload,
        ..Reply::mutated(1, revision + 1)
    })
}

/// `table.clear(array)`: static first; a mapped array is dirty even when it was empty.
pub fn value_array_clear(owner: KilnOwner, array: u64) -> Result<Reply, Refusal> {
    contained(|| {
        let (local, _scope, held) = enter_lease(array, owner)?;
        let node = array_or_unsupported!(held);
        let revision = node.revision.get();
        let _borrow = step!(&local, revision, node.borrow.exclusive());
        step!(&local, revision, node.clear());
        node.revision.set(revision + 1);
        Ok(Reply::mutated(1, revision + 1))
    })
}

// ---- startup apply ----

/// One edited RESOURCE root for the startup writer: its key and its value converted (or the conversion's Source error).
pub type DirtyRoot = (KilnAssetKey, Result<Value, Fault>);

/// The STARTUP arena's edited RESOURCE roots as kfc values, for the single writer's apply: SF's
/// `ResourceInfo::apply` over the cached roots whose value is dirty. Loaded, fresh, cloned and detached values never
/// enroll, and a RUNTIME arena has nothing to apply (UNAVAILABLE).
pub fn startup_dirty_roots(host_owner: KilnOwner, control: u64) -> Result<Vec<DirtyRoot>, Refusal> {
    contained(|| {
        let (local, _scope) = enter_control(control, host_owner)?;
        if local.phase != KILN_VALUE_STARTUP as u32 {
            return Err(Refusal::Unavailable);
        }
        let roots: Vec<(RootKey, Val)> = local
            .roots
            .borrow()
            .iter()
            .map(|(key, slot)| (*key, slot.val.clone()))
            .collect();
        let mut out = Vec::new();
        for (key, val) in roots {
            let (RootKey::Resource { guid, r#type, part }, Val::Node(node)) = (key, &val) else {
                continue;
            };
            let asset = KilnAssetKey { guid, r#type, part };
            let converted = match is_dirty(&val, 0) {
                Ok(false) => continue,
                Ok(true) => match node.classify() {
                    Some((_, declared)) => Converter::convert(&val, &declared.clone(), &*local),
                    None => continue,
                },
                Err(fault) => Err(fault),
            };
            out.push((asset, converted));
        }
        Ok(out)
    })
}
