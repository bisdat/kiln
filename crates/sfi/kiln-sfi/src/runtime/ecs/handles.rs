// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/game-runtime/native/ecs/resolve_entity_handle.cpp:4-27 (`handle_for`: one opaque
// handle per entity id and generation, never recycled in the process), :38-91 (`entity_for_handle`: a handle names
// that id and generation in its layout epoch only, and every use reads the live identity again).
//! ShroudForge's opaque entity handles on kiln.world's self-validating refs.
//!
//! A ShroudForge mod never sees an engine entity: `runtime.ecs.query` and `resolve` give it opaque u32 handles, and
//! every read or write takes one back. Kiln's side of a handle is the [`KilnObjectRef`] kiln.world gave for it
//! (entity id, generation, world kind, world session, layout stamp, reload epoch), which kiln.world validates again on
//! every use; the table here only maps the number to that ref.
//!
//! - **One handle per identity**: the same entity id and generation, in the same world kind and session, always maps
//!   to the same handle (a query and a `resolve` of the same entity agree), and the stored ref is refreshed to the
//!   newest one seen for it.
//! - **Never recycled**: numbers come from one process-wide counter starting at 1, so no handle names two entities in
//!   the life of the process, across worlds, sessions and live reloads (another VM's handle is simply unknown here).
//!   When the u32 space is used up, no new handle is given (0), as ShroudForge's counter stops.
//! - **Stale handles**: a handle of another world or session is forgotten as soon as a ref of the new one is seen (a
//!   client that stops hosting and joins another world, or a new session: it could never validate again); an unknown handle reads as nothing ([`Handles::get`] is `None`), which each
//!   operation answers with its own ShroudForge refusal. A handle of the current session whose entity died, or whose
//!   id now names a newer generation, keeps its ref: kiln.world answers it stale or absent on use.
//!
//! The table belongs to one [`super::Ecs`] (one VM), on the worker thread. Other families (the world cursor, props)
//! hand their entities to mods through [`Handles::handle_for`] the same way, so a mod can pass them on to
//! `runtime.ecs.read`.

use kiln_abi::KilnEntity;
use kiln_abi::services::kiln_world::KilnObjectRef;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::sync::atomic::{AtomicU32, Ordering};

/// The process's next handle (0: the space is used up).
static NEXT: AtomicU32 = AtomicU32::new(1);

/// A fresh handle, never given before in this process; 0 once the u32 space is used up.
fn fresh() -> u32 {
    let mut next = NEXT.load(Ordering::Relaxed);
    loop {
        if next == 0 {
            return 0;
        }
        match NEXT.compare_exchange_weak(
            next,
            next.wrapping_add(1),
            Ordering::Relaxed,
            Ordering::Relaxed,
        ) {
            Ok(_) => return next,
            Err(seen) => next = seen,
        }
    }
}

/// What one handle names: an entity id and generation in one world kind and session.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct Identity {
    world_kind: u32,
    entity: KilnEntity,
    generation: u32,
    session: u64,
}
impl Identity {
    fn of(object: &KilnObjectRef) -> Self {
        Self {
            world_kind: object.world_kind,
            entity: object.entity,
            generation: object.generation,
            session: object.session,
        }
    }
}

/// The handles one VM gave out (module docs).
#[derive(Default)]
pub struct Handles {
    refs: RefCell<HashMap<u32, KilnObjectRef>>,
    by_identity: RefCell<HashMap<Identity, u32>>,
    /// The world kind and session of the newest ref seen (the handles of every other world and session were dropped).
    world: Cell<(u32, u64)>,
}

impl Handles {
    pub fn new() -> Self {
        Self::default()
    }

    /// The handle of `object`: the one already given for its entity id, generation, world kind and session, else a
    /// new one. 0 when the process's handle space is used up (nothing is recorded then).
    pub fn handle_for(&self, object: &KilnObjectRef) -> u32 {
        if (object.world_kind, object.session) != self.world.get() {
            self.forget_other_worlds(object.world_kind, object.session);
        }
        let identity = Identity::of(object);
        if let Some(handle) = self.by_identity.borrow().get(&identity).copied() {
            self.refs.borrow_mut().insert(handle, *object);
            return handle;
        }
        let handle = fresh();
        if handle == 0 {
            return 0;
        }
        self.by_identity.borrow_mut().insert(identity, handle);
        self.refs.borrow_mut().insert(handle, *object);
        handle
    }

    /// The ref handle `handle` names, if this table gave it and its world and session are the newest seen.
    pub fn get(&self, handle: u32) -> Option<KilnObjectRef> {
        self.refs.borrow().get(&handle).copied()
    }

    /// How many handles are known now.
    pub fn len(&self) -> usize {
        self.refs.borrow().len()
    }
    pub fn is_empty(&self) -> bool {
        self.refs.borrow().is_empty()
    }

    fn forget_other_worlds(&self, world_kind: u32, session: u64) {
        self.refs
            .borrow_mut()
            .retain(|_, object| object.world_kind == world_kind && object.session == session);
        self.by_identity
            .borrow_mut()
            .retain(|identity, _| identity.world_kind == world_kind && identity.session == session);
        self.world.set((world_kind, session));
    }
}
