// SPDX-License-Identifier: GPL-3.0-or-later
//! Arenas and tokens of the typed graph.
//!
//! # Tokens
//! Every token (an arena's identity and control, value, iterator and packet leases) comes from one process-wide counter:
//! nonzero, never reused and never a pointer. A token below the counter was issued; one at or above it never was
//! (NOT_FOUND). The live ones sit in one process-wide table (token → arena and kind); a closed or revoked token leaves
//! that table, and later uses read REVOKED while its close stays idempotent. The table lock is held only for the table
//! itself, never across a host call, Lua or a graph walk.
//!
//! # The two halves of an arena
//! - [`ArenaShared`] is any-thread: the closed flag, the queue of closed leases and the count of host objects still
//!   retained. `value_close` and `arena_close` work here only: they never run Lua and never free graph state off the worker;
//!   off the worker they signal the host (`wake`) to call `arena_collect`. Once a closed arena is torn down and nothing
//!   is pending, the host may free its reader, so the waker is dropped then and no later close signals it.
//! - [`ArenaLocal`] is the creator worker's: the registry, the host reader, the lease map holding nodes, iterators and
//!   packets, the ResourceInfo roots, and the release queue. It lives in a thread-local map, so a worker-only call on
//!   another thread finds nothing and is refused WRONG_THREAD.
//!
//! # Scopes
//! Every worker call runs in a [`Scope`]. Temporary host tokens live until the outermost scope ends, so the graph
//! retains (pins) what it keeps before that. A close that arrives inside a scope (a GC finalizer during a reader
//! callback) is only queued; when the outermost scope ends the arena drains once: it drops the closed leases, then
//! releases the queued host objects, repeating until both queues are empty. Nothing is undone and nothing is applied
//! under an outer operation.
//!
//! # Worker exit
//! The host closes and drains every arena before its worker exits (and before it destroys the VM). A worker that exits
//! with an arena still open revokes it as its thread's locals go: every token is revoked (later closes from other
//! threads are idempotent, `arena_close`/`arena_collect` read REVOKED), the waker is dropped so nothing signals a host
//! that may be gone, and the graph is dropped without calling the host at all (the objects it retained for the graph
//! die with its VM).
//!
//! # Lifetime authority
//! A node lives while a lease, a cached slot or a root record holds it. A LOADED_SNAPSHOT root's record is weak:
//! Source keeps that root's value on the loaded ResourceInfo itself, so the frontend keeps the root's userdata on its
//! ResourceInfo and the record frees with it; a later open of the same instance while it lives returns the same node. A call's named owner is attribution only: a
//! package's retirement has no effect here, and a reference another mod holds keeps working. Only `arena_close` revokes
//! every lease; the worker then tears the arena down at its next collect, releasing every retained host object before
//! the host may destroy its VM (pending reaches 0).
use std::{
    cell::{Cell, RefCell},
    collections::{HashMap, HashSet},
    rc::{Rc, Weak},
    sync::{
        Arc, LazyLock, Mutex, MutexGuard,
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
    },
    thread::ThreadId,
};

use kfc::reflection::TypeRegistry;
use kiln_abi::{
    KilnOwner,
    services::{kiln_assets::KilnAssetKey, kiln_eml_archive::KilnEmlArchiveIdentityV1},
};

use super::{
    lifetime::{Fault, LuaHost, Pin, Refusal, Sink, Waker},
    node::{Env, Iter, Node, Slot},
};

static NEXT: AtomicU64 = AtomicU64::new(1);

/// A new process-unique token.
fn issue() -> u64 {
    NEXT.fetch_add(1, Ordering::SeqCst)
}

/// Whether `token` was ever issued.
pub(crate) fn was_issued(token: u64) -> bool {
    token != 0 && token < NEXT.load(Ordering::SeqCst)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum TokenKind {
    /// The creator host's control lease (arena_create's reply).
    Control,
    Value,
    Iterator,
    Packet,
}

struct TokenEntry {
    arena: u64,
    kind: TokenKind,
}

struct ArenaEntry {
    shared: Arc<ArenaShared>,
    /// The live leases (not the control token).
    leases: HashSet<u64>,
}

#[derive(Default)]
struct Table {
    tokens: HashMap<u64, TokenEntry>,
    arenas: HashMap<u64, ArenaEntry>,
}

static TABLE: LazyLock<Mutex<Table>> = LazyLock::new(|| Mutex::new(Table::default()));

fn table() -> MutexGuard<'static, Table> {
    TABLE.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// The any-thread half of an arena.
pub(crate) struct ArenaShared {
    /// The public identity (`KilnEmlValueIdentityV1.arena`, MAKE_TYPED_PROXY's arena): not a lease.
    pub(crate) id: u64,
    pub(crate) control: u64,
    pub(crate) host_owner: KilnOwner,
    pub(crate) creator: ThreadId,
    pub(crate) closed: AtomicBool,
    /// Closed leases the worker has not dropped yet, and the host's wake signal.
    queue: Mutex<Queue>,
    /// Host objects retained and not yet released.
    pub(crate) retained: Arc<AtomicUsize>,
}

#[derive(Default)]
struct Queue {
    closes: Vec<u64>,
    /// `reader.wake`; None once the worker tore the arena down and nothing was pending: the host may then free its
    /// reader, so no later close (another thread's `arena_close` or `value_close`) may signal it.
    waker: Option<Waker>,
}

impl ArenaShared {
    fn queue(&self) -> MutexGuard<'_, Queue> {
        self.queue.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
    /// Queues a closed lease (if any) and, when asked, signals the host, both under the queue's lock: the teardown
    /// takes the waker under that lock before it reports pending 0, so no signal can follow that report. `wake` runs
    /// no Lua and must not call back into this service.
    fn queue_close(&self, token: Option<u64>, wake: bool) {
        let mut queue = self.queue();
        if let Some(token) = token {
            queue.closes.push(token);
        }
        if wake && let Some(waker) = &queue.waker {
            waker();
        }
    }
    /// Closed leases still to drop plus host objects still retained: the host keeps its reader until this is 0.
    pub(crate) fn pending(&self) -> u32 {
        let total = self.queue().closes.len() + self.retained.load(Ordering::SeqCst);
        total.min(u32::MAX as usize) as u32
    }
    /// `pending` once the worker half is gone (torn down): a close another thread queued after the teardown names a
    /// lease the teardown already dropped, so it is discarded instead of counting forever.
    fn settled(&self) -> u32 {
        self.queue().closes.clear();
        self.pending()
    }
    fn on_creator(&self) -> bool {
        std::thread::current().id() == self.creator
    }
}

/// The pins an arena was created with: a copy of its archive view's identity and the registry instance's revision.
#[derive(Clone, Copy)]
pub struct ArenaPins {
    pub archive: KilnEmlArchiveIdentityV1,
    pub layout_revision: u64,
}

/// Where a RESOURCE root's bytes come from (the arena's archive generation; tests: a fixture).
pub trait ResourceSource {
    /// The resource's bytes and its type's registry index; None when the archive has no such resource (Source's root
    /// is then nil). An error is the text Source's read raises.
    fn read(&self, key: &KilnAssetKey) -> Result<Option<(Vec<u8>, usize)>, String>;
}

/// Everything `arena_create` needs; the C table resolves the archive view to the registry and pins (
/// §5) and adapts the C reader to [`LuaHost`].
pub struct ArenaConfig {
    pub registry: Rc<TypeRegistry>,
    pub pins: ArenaPins,
    /// `KILN_VALUE_STARTUP` or `KILN_VALUE_RUNTIME`.
    pub phase: u32,
    pub host: Rc<dyn LuaHost>,
    pub waker: Option<Waker>,
    pub resources: Option<Rc<dyn ResourceSource>>,
}

/// What a lease holds.
pub(crate) enum Lease {
    /// A node; `pin` keeps the userdata a removal handed back until the caller has it.
    Value {
        node: Rc<Node>,
        _pin: Option<Pin>,
    },
    Iterator(Rc<Iter>),
    Packet(Rc<Packet>),
}

/// What a lease holds, without its pin, for one call.
#[derive(Clone)]
pub(crate) enum Held {
    Value(Rc<Node>),
    Iterator(Rc<Iter>),
    Packet(Rc<Packet>),
}

/// An immutable result: a payload's or an error's bytes, and the host errors an error keeps.
pub(crate) struct Packet {
    pub(crate) error: bool,
    pub(crate) bytes: Vec<u8>,
    pub(crate) _pins: Vec<Pin>,
}

/// A root record: a RESOURCE root by asset key, a LOADED_SNAPSHOT root by its ResourceInfo instance.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum RootKey {
    Resource { guid: [u8; 16], r#type: u32, part: u32 },
    Loaded { instance: u64, session: u64 },
}

/// The creator worker's half of an arena.
pub(crate) struct ArenaLocal {
    pub(crate) shared: Arc<ArenaShared>,
    pub(crate) registry: Rc<TypeRegistry>,
    pub(crate) pins: ArenaPins,
    pub(crate) phase: u32,
    pub(crate) host: Rc<dyn LuaHost>,
    pub(crate) resources: Option<Rc<dyn ResourceSource>>,
    pub(crate) sink: Rc<Sink>,
    pub(crate) leases: RefCell<HashMap<u64, Lease>>,
    /// Source's ResourceInfo value caches of RESOURCE roots: each pins its root's userdata.
    pub(crate) roots: RefCell<HashMap<RootKey, Slot>>,
    /// LOADED_SNAPSHOT roots by ResourceInfo instance, weak and unpinned (the frontend's ResourceInfo keeps the root).
    loaded: RefCell<HashMap<RootKey, Weak<Node>>>,
    /// `loaded`'s size at which its dead records are swept next.
    loaded_sweep: Cell<usize>,
    depth: Cell<u32>,
    draining: Cell<bool>,
    torn_down: Cell<bool>,
    next_node: Cell<u64>,
    /// The current call's named owner (attribution of the nodes it makes).
    caller: Cell<KilnOwner>,
}

/// The worker halves of this thread's arenas. Dropped when the thread exits: an arena still open then is abandoned
/// (see "Worker exit").
#[derive(Default)]
struct Locals(HashMap<u64, Rc<ArenaLocal>>);

impl Drop for Locals {
    fn drop(&mut self) {
        for local in self.0.values() {
            local.abandon();
        }
    }
}

thread_local! {
    static LOCALS: RefCell<Locals> = RefCell::new(Locals::default());
}

/// The worker half of an arena on this thread; None off its worker, and while the thread's locals are being destroyed
/// (a finalizer of the dying VM may still close a lease then).
fn local_of(arena: u64) -> Option<Rc<ArenaLocal>> {
    LOCALS
        .try_with(|locals| {
            locals
                .try_borrow()
                .ok()
                .and_then(|locals| locals.0.get(&arena).cloned())
        })
        .ok()
        .flatten()
}

/// Creates an arena on the calling thread, its creator worker, and returns its control token.
pub fn arena_create(host_owner: KilnOwner, config: ArenaConfig) -> Result<u64, Refusal> {
    use kiln_abi::services::kiln_eml_values::{KILN_VALUE_RUNTIME, KILN_VALUE_STARTUP};
    if config.phase != KILN_VALUE_STARTUP as u32 && config.phase != KILN_VALUE_RUNTIME as u32 {
        return Err(Refusal::InvalidArgument);
    }
    let retained = Arc::new(AtomicUsize::new(0));
    let shared = Arc::new(ArenaShared {
        id: issue(),
        control: issue(),
        host_owner,
        creator: std::thread::current().id(),
        closed: AtomicBool::new(false),
        queue: Mutex::new(Queue {
            closes: Vec::new(),
            waker: config.waker,
        }),
        retained: retained.clone(),
    });
    let local = Rc::new(ArenaLocal {
        shared: shared.clone(),
        registry: config.registry,
        pins: config.pins,
        phase: config.phase,
        host: config.host,
        resources: config.resources,
        sink: Rc::new(Sink::new(retained)),
        leases: RefCell::new(HashMap::new()),
        roots: RefCell::new(HashMap::new()),
        loaded: RefCell::new(HashMap::new()),
        loaded_sweep: Cell::new(64),
        depth: Cell::new(0),
        draining: Cell::new(false),
        torn_down: Cell::new(false),
        next_node: Cell::new(1),
        caller: Cell::new(host_owner),
    });
    {
        let mut table = table();
        table.tokens.insert(
            shared.control,
            TokenEntry {
                arena: shared.id,
                kind: TokenKind::Control,
            },
        );
        table.arenas.insert(
            shared.id,
            ArenaEntry {
                shared: shared.clone(),
                leases: HashSet::new(),
            },
        );
    }
    LOCALS.with(|locals| locals.borrow_mut().0.insert(shared.id, local));
    Ok(shared.control)
}

/// The arena and kind of a live token.
pub(crate) fn lookup(token: u64) -> Result<(Arc<ArenaShared>, TokenKind), Refusal> {
    let table = table();
    match table.tokens.get(&token) {
        Some(entry) => {
            let arena = table.arenas.get(&entry.arena).ok_or(Refusal::Internal)?;
            Ok((arena.shared.clone(), entry.kind))
        }
        None if was_issued(token) => Err(Refusal::Revoked),
        None => Err(Refusal::NotFound),
    }
}

/// Any-thread: revokes every lease of the arena. Idempotent; the control token stays valid for `arena_collect`.
pub fn arena_close(_host_owner: KilnOwner, control: u64) -> Result<u32, Refusal> {
    let (shared, kind) = lookup(control)?;
    if kind != TokenKind::Control {
        return Err(Refusal::InvalidArgument);
    }
    let revoked = {
        let mut table = table();
        shared.closed.store(true, Ordering::SeqCst);
        let leases = match table.arenas.get_mut(&shared.id) {
            Some(entry) => std::mem::take(&mut entry.leases),
            None => HashSet::new(),
        };
        for token in &leases {
            table.tokens.remove(token);
        }
        leases.len()
    };
    if shared.on_creator() {
        // inside a call (a callback closed it) the teardown waits for the outermost call's end
        return Ok(match local_of(shared.id) {
            Some(local) => {
                local.drain();
                local.pending()
            }
            None => shared.settled(),
        });
    }
    shared.queue_close(None, true);
    // off the worker: the revoked leases still hold graph state until the worker's collect
    let pending = shared.pending() as usize + revoked;
    Ok(pending.min(u32::MAX as usize) as u32)
}

/// Worker: drops closed leases and releases queued host objects; after `arena_close`, tears the arena down. Returns
/// what is still pending (0 once a closed arena released everything).
pub fn arena_collect(_host_owner: KilnOwner, control: u64) -> Result<u32, Refusal> {
    let (shared, kind) = lookup(control)?;
    if kind != TokenKind::Control {
        return Err(Refusal::InvalidArgument);
    }
    if !shared.on_creator() {
        return Err(Refusal::WrongThread);
    }
    Ok(match local_of(shared.id) {
        Some(local) => {
            local.drain();
            local.pending()
        }
        None => shared.settled(),
    })
}

/// Any-thread: closes one lease. Idempotent for an issued token; NOT_FOUND for one never issued. On its worker outside
/// any call it drains at once; inside a call (a finalizer during a reader callback) the drain waits for the outermost
/// call's end; off the worker it is queued and the host is woken.
pub fn value_close(_owner: KilnOwner, token: u64) -> Result<(), Refusal> {
    if !was_issued(token) {
        return Err(Refusal::NotFound);
    }
    let shared = {
        let mut table = table();
        match table.tokens.get(&token) {
            None => return Ok(()),
            Some(entry) if entry.kind == TokenKind::Control => return Err(Refusal::InvalidArgument),
            Some(_) => {}
        }
        let entry = table.tokens.remove(&token).expect("present");
        let arena = table.arenas.get_mut(&entry.arena).ok_or(Refusal::Internal)?;
        arena.leases.remove(&token);
        arena.shared.clone()
    };
    let on_creator = shared.on_creator();
    shared.queue_close(Some(token), !on_creator);
    if on_creator && let Some(local) = local_of(shared.id) {
        local.drain();
    }
    Ok(())
}

/// One worker call on an arena: entered by every worker operation.
pub(crate) struct Scope {
    local: Rc<ArenaLocal>,
    caller: KilnOwner,
}

impl Drop for Scope {
    fn drop(&mut self) {
        let depth = self.local.depth.get() - 1;
        self.local.depth.set(depth);
        self.local.caller.set(self.caller);
        if depth == 0 {
            self.local.drain();
        }
    }
}

/// The worker half of a live arena for a worker-only call: REVOKED when it is closed, WRONG_THREAD off its worker.
pub(crate) fn enter(shared: &ArenaShared, owner: KilnOwner) -> Result<(Rc<ArenaLocal>, Scope), Refusal> {
    if shared.closed.load(Ordering::SeqCst) {
        return Err(Refusal::Revoked);
    }
    if !shared.on_creator() {
        return Err(Refusal::WrongThread);
    }
    let local = local_of(shared.id).ok_or(Refusal::Revoked)?;
    local.depth.set(local.depth.get() + 1);
    let caller = local.caller.replace(owner);
    let scope = Scope {
        local: local.clone(),
        caller,
    };
    Ok((local, scope))
}

/// The arena behind a control token, entered for a worker call.
pub(crate) fn enter_control(control: u64, owner: KilnOwner) -> Result<(Rc<ArenaLocal>, Scope), Refusal> {
    let (shared, kind) = lookup(control)?;
    if kind != TokenKind::Control {
        return Err(Refusal::InvalidArgument);
    }
    enter(&shared, owner)
}

/// A lease's arena entered for a worker call, and what the lease holds.
pub(crate) fn enter_lease(token: u64, owner: KilnOwner) -> Result<(Rc<ArenaLocal>, Scope, Held), Refusal> {
    let (shared, kind) = lookup(token)?;
    if kind == TokenKind::Control {
        return Err(Refusal::InvalidArgument);
    }
    let (local, scope) = enter(&shared, owner)?;
    let held = local.held(token).ok_or(Refusal::Revoked)?;
    Ok((local, scope, held))
}

impl ArenaLocal {
    pub(crate) fn id(&self) -> u64 {
        self.shared.id
    }

    pub(crate) fn closed(&self) -> bool {
        self.shared.closed.load(Ordering::SeqCst)
    }

    pub(crate) fn held(&self, token: u64) -> Option<Held> {
        self.leases.borrow().get(&token).map(|lease| match lease {
            Lease::Value { node, .. } => Held::Value(node.clone()),
            Lease::Iterator(iter) => Held::Iterator(iter.clone()),
            Lease::Packet(packet) => Held::Packet(packet.clone()),
        })
    }

    /// A value lease of THIS arena (a typed value the host passed back): None for any other token.
    pub(crate) fn own_value(&self, token: u64) -> Option<Rc<Node>> {
        let (shared, kind) = lookup(token).ok()?;
        if shared.id != self.shared.id || kind != TokenKind::Value {
            return None;
        }
        match self.held(token)? {
            Held::Value(node) => Some(node),
            _ => None,
        }
    }

    /// Issues a lease: REVOKED once the arena is closed (nothing is then held).
    pub(crate) fn issue(&self, kind: TokenKind, lease: Lease) -> Result<u64, Refusal> {
        let token = {
            let mut table = table();
            if self.closed() {
                return Err(Refusal::Revoked);
            }
            let token = issue();
            table.tokens.insert(
                token,
                TokenEntry {
                    arena: self.shared.id,
                    kind,
                },
            );
            table
                .arenas
                .get_mut(&self.shared.id)
                .ok_or(Refusal::Internal)?
                .leases
                .insert(token);
            token
        };
        self.leases.borrow_mut().insert(token, lease);
        Ok(token)
    }

    pub(crate) fn issue_node(&self, node: Rc<Node>, pin: Option<Pin>) -> Result<u64, Refusal> {
        self.issue(TokenKind::Value, Lease::Value { node, _pin: pin })
    }

    pub(crate) fn issue_packet(&self, bytes: Vec<u8>, error: bool, pins: Vec<Pin>) -> Result<u64, Refusal> {
        self.issue(
            TokenKind::Packet,
            Lease::Packet(Rc::new(Packet {
                error,
                bytes,
                _pins: pins,
            })),
        )
    }

    pub(crate) fn issue_iterator(&self, iter: Iter) -> Result<u64, Refusal> {
        self.issue(TokenKind::Iterator, Lease::Iterator(Rc::new(iter)))
    }

    /// Withdraws a lease this call reserved and will not return (its contents drop at the drain).
    pub(crate) fn withdraw(&self, token: u64) {
        if token == 0 {
            return;
        }
        {
            let mut table = table();
            if table.tokens.remove(&token).is_some()
                && let Some(arena) = table.arenas.get_mut(&self.shared.id)
            {
                arena.leases.remove(&token);
            }
        }
        let dropped = self.leases.borrow_mut().remove(&token);
        drop(dropped);
    }

    /// Moves a pin into an issued value lease (a removed value's userdata, kept until the caller has it).
    pub(crate) fn attach_pin(&self, token: u64, pin: Pin) {
        if let Some(Lease::Value { _pin: slot, .. }) = self.leases.borrow_mut().get_mut(&token) {
            *slot = Some(pin);
        }
    }

    /// Drops closed leases and releases queued host objects, repeatedly, once no call is in progress. A closed arena is
    /// torn down here: every lease and root drops, every retained host object is released, then its worker half goes.
    pub(crate) fn drain(&self) {
        if self.depth.get() > 0 || self.draining.replace(true) {
            return;
        }
        loop {
            let closes = std::mem::take(&mut self.shared.queue().closes);
            let mut dropped: Vec<Lease> = Vec::new();
            {
                let mut leases = self.leases.borrow_mut();
                for token in &closes {
                    if let Some(lease) = leases.remove(token) {
                        dropped.push(lease);
                    }
                }
            }
            drop(dropped);
            let mut torn = false;
            if self.closed() && !self.torn_down.get() {
                self.torn_down.set(true);
                let leases = std::mem::take(&mut *self.leases.borrow_mut());
                let roots = std::mem::take(&mut *self.roots.borrow_mut());
                self.loaded.borrow_mut().clear();
                drop(leases);
                drop(roots);
                torn = true;
            }
            let releases = self.sink.take();
            for object in &releases {
                self.host.release(*object);
                self.sink.released();
            }
            if closes.is_empty() && releases.is_empty() && !torn {
                break;
            }
        }
        self.draining.set(false);
        if self.torn_down.get() && self.shared.pending() == 0 {
            // the host may now destroy its VM and free its reader: nothing here calls it again, not even `wake`
            let waker = self.shared.queue().waker.take();
            drop(waker);
            let _ = LOCALS.try_with(|locals| {
                if let Ok(mut locals) = locals.try_borrow_mut() {
                    locals.0.remove(&self.shared.id);
                }
            });
        }
    }

    /// The live LOADED_SNAPSHOT root of a ResourceInfo instance, if its frontend still holds it.
    pub(crate) fn loaded_root(&self, key: &RootKey) -> Option<Rc<Node>> {
        self.loaded.borrow().get(key).and_then(Weak::upgrade)
    }

    /// Records a LOADED_SNAPSHOT root weakly; dead records are swept whenever the map has doubled since the last sweep.
    pub(crate) fn keep_loaded(&self, key: RootKey, node: &Rc<Node>) {
        let mut loaded = self.loaded.borrow_mut();
        loaded.insert(key, Rc::downgrade(node));
        if loaded.len() >= self.loaded_sweep.get() {
            loaded.retain(|_, root| root.strong_count() > 0);
            self.loaded_sweep.set((loaded.len() * 2).max(64));
        }
    }

    /// The worker exits with this arena open (see "Worker exit"): every token is revoked, the waker goes, and the graph
    /// drops without a single host call (queued releases are discarded).
    fn abandon(&self) {
        {
            let mut table = table();
            self.shared.closed.store(true, Ordering::SeqCst);
            table.tokens.remove(&self.shared.control);
            let leases = table
                .arenas
                .get_mut(&self.shared.id)
                .map(|entry| std::mem::take(&mut entry.leases))
                .unwrap_or_default();
            for token in leases {
                table.tokens.remove(&token);
            }
        }
        let waker = {
            let mut queue = self.shared.queue();
            queue.closes.clear();
            queue.waker.take()
        };
        drop(waker);
        self.torn_down.set(true);
        let leases = std::mem::take(&mut *self.leases.borrow_mut());
        let roots = std::mem::take(&mut *self.roots.borrow_mut());
        self.loaded.borrow_mut().clear();
        drop(leases);
        drop(roots);
        let discarded = self.sink.take();
        for _ in &discarded {
            self.sink.released();
        }
    }

    /// What is still pending: closed leases and retained host objects, plus, for a closed arena not yet torn down, the
    /// leases and roots its teardown will drop.
    pub(crate) fn pending(&self) -> u32 {
        let mut total = self.shared.pending() as usize;
        if self.closed() && !self.torn_down.get() {
            total += self.leases.borrow().len() + self.roots.borrow().len();
        }
        total.min(u32::MAX as usize) as u32
    }
}

impl Env for ArenaLocal {
    fn owner(&self) -> KilnOwner {
        self.caller.get()
    }
    fn new_id(&self) -> u64 {
        let id = self.next_node.get();
        self.next_node.set(id + 1);
        id
    }
    fn userdata_text(&self, node: &Rc<Node>) -> Result<Vec<u8>, Fault> {
        super::operations::proxy_text(self, node)
    }
}
