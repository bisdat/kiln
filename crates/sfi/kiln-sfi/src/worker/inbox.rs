// SPDX-License-Identifier: MIT
//! The command seam into the interpreter worker.
//!
//! A producer (the tick, a world thread, the window thread, kiln-sfi's lifecycle
//! subscriber, Kiln.Ui's bridge) never blocks and never waits on the worker: there
//! is no mutex here that a producer could find held by the below-normal worker.
//! - The event class (`UiAction`) goes through one bounded lock-free ring; a full
//!   ring refuses at the producer and counts the drop.
//! - State classes (`ConfigurationObservation` per package, `InputSnapshot`,
//!   `NetworkAuth`, `ReloadRequest`) keep only the latest pending value per key: a newer value replaces
//!   (coalesces) the older one, so they never overflow while the key table has room.
//! - `WorldLifecycle` is never dropped: one slot per world kind keeps the newest
//!   event, and the drain expands it to at most two events per world.
//!
//! The single consumer is the worker body; [`Inbox::consumer`] hands out exactly
//! one [`Consumer`] per inbox, and that token is `!Send`.

use std::cell::UnsafeCell;
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU8, AtomicU64, AtomicUsize, Ordering};

/// `KILN_WORLD_KINDS` (server, prediction, client).
pub const WORLD_KINDS: usize = 3;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum LifecycleEvent {
    Started,
    Stopping,
}

/// A world's lifecycle as kiln-sfi's `subscribe_world` callback saw it. `world` is
/// the world kind (`KILN_WORLD_*`), `session` is `KilnWorldEvent.session` (from 1).
/// Never a world pointer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WorldLifecycle {
    pub world: u32,
    pub session: u32,
    pub event: LifecycleEvent,
}

/// A Kiln.Ui action for one package (the UI bridge defines the payload).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UiAction {
    pub package: String,
    pub action: String,
    pub payload: Vec<u8>,
}

/// Input state (the input feed defines its fields). A state class: only the latest counts.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InputSnapshot {
    pub payload: Vec<u8>,
}

/// The dedicated server log follower's latest authorization state (kiln-sfi's IO
/// thread). A state class: only the latest counts. ShroudForge's network
/// API is pull-based, so no inbound message ever comes through the inbox (it replaces the
/// unused `NetworkInbound` event class, which keeps the class count at 6).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NetworkAuth {
    /// Counts the follower's publications (from 1).
    pub generation: u64,
    /// The log could be read; `peers` is empty when it could not.
    pub readable: bool,
    /// Live and authenticated server peers (Steam ids), ascending, each once.
    pub peers: Vec<u64>,
}

/// The latest live reload/unload request kiln-sfi's IO thread read from
/// `<game>/kiln/sfi/runtime/mod-reload-request.json` (ShroudForge's request document, parsed; a file
/// that could not be read or parsed never gets here). A state class: only the latest counts, as
/// ShroudForge reads only the file's current content at its next update. The worker deduplicates on
/// `requestId` (sfi-core's `ReloadProtocol`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ReloadRequest {
    pub request: serde_json::Value,
}

/// One package's configuration observation. A state class keyed by `package`.
/// The lifecycle host maps it onto sfi-core's observation when it applies it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConfigurationObservation {
    pub package: String,
    pub payload: Vec<u8>,
}

/// A closed set of owned, Send commands. Never a game pointer, a Lua value,
/// a borrowed slice or a callback. New commands append variants.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum WorkerCommand {
    WorldLifecycle(WorldLifecycle),
    ConfigurationObservation(ConfigurationObservation),
    UiAction(UiAction),
    InputSnapshot(InputSnapshot),
    NetworkAuth(NetworkAuth),
    ReloadRequest(ReloadRequest),
    /// kiln-sfi's own orderly stop request (sets `WorkerShared::stop`).
    Stop,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommandClass {
    WorldLifecycle = 0,
    ConfigurationObservation = 1,
    UiAction = 2,
    InputSnapshot = 3,
    NetworkAuth = 4,
    Stop = 5,
    ReloadRequest = 6,
}
pub const COMMAND_CLASSES: usize = 7;

impl WorkerCommand {
    pub fn class(&self) -> CommandClass {
        match self {
            Self::WorldLifecycle(_) => CommandClass::WorldLifecycle,
            Self::ConfigurationObservation(_) => CommandClass::ConfigurationObservation,
            Self::UiAction(_) => CommandClass::UiAction,
            Self::InputSnapshot(_) => CommandClass::InputSnapshot,
            Self::NetworkAuth(_) => CommandClass::NetworkAuth,
            Self::ReloadRequest(_) => CommandClass::ReloadRequest,
            Self::Stop => CommandClass::Stop,
        }
    }
}

/// What a producer's push did. Every variant means the command is (or is folded
/// into) pending work; none of them waited.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Accepted {
    Queued,
    /// A state class replaced an older pending value of the same key.
    Coalesced,
    /// A lifecycle event folded into a newer (or replaced an older) pending one.
    Superseded,
}

/// A producer-side refusal, counted per class. The caller answers with
/// ShroudForge's failure shape for its operation where SF has one.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refused {
    /// An event class's ring is full, or a state class's key table is full.
    Full(CommandClass),
    /// Malformed command (a world kind out of range, session 0).
    Argument,
    /// The worker has stopped; nothing will drain this inbox again.
    Stopped,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InboxConfig {
    /// Pending `UiAction`s (rounded up to a power of two). Kiln.Ui keeps at most
    /// 1,024 pending actions per mod; the per-mod split is the UI
    /// bridge's, this is the ring's own bound.
    pub ui_actions: usize,
    /// Distinct `ConfigurationObservation` packages that can be pending at once.
    pub configuration_keys: usize,
}
impl Default for InboxConfig {
    fn default() -> Self {
        Self::DEFAULT
    }
}
impl InboxConfig {
    pub const DEFAULT: Self = Self {
        ui_actions: 1024,
        configuration_keys: 256,
    };
}

/// Per-class producer counters, readable from any thread (status).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InboxCounters {
    pub accepted: [u64; COMMAND_CLASSES],
    pub coalesced: [u64; COMMAND_CLASSES],
    pub refused: [u64; COMMAND_CLASSES],
    pub delivered: [u64; COMMAND_CLASSES],
}

pub struct Inbox {
    ui: Ring<UiAction>,
    network: Coalescer<NetworkAuth>,
    reload: Coalescer<ReloadRequest>,
    configuration: Coalescer<ConfigurationObservation>,
    input: Coalescer<InputSnapshot>,
    lifecycle: [AtomicU64; WORLD_KINDS],
    consumer_claimed: AtomicBool,
    accepted: [AtomicU64; COMMAND_CLASSES],
    coalesced: [AtomicU64; COMMAND_CLASSES],
    refused: [AtomicU64; COMMAND_CLASSES],
    delivered: [AtomicU64; COMMAND_CLASSES],
}

impl Inbox {
    pub fn new(config: InboxConfig) -> Self {
        Self {
            ui: Ring::new(config.ui_actions),
            network: Coalescer::new(1),
            reload: Coalescer::new(1),
            configuration: Coalescer::new(config.configuration_keys),
            input: Coalescer::new(1),
            lifecycle: Default::default(),
            consumer_claimed: AtomicBool::new(false),
            accepted: Default::default(),
            coalesced: Default::default(),
            refused: Default::default(),
            delivered: Default::default(),
        }
    }

    /// Producer side, any thread, never blocks. `Stop` is not an inbox item (the
    /// caller sets the stop flag); it is refused here as `Argument`.
    pub fn push(&self, command: WorkerCommand) -> Result<Accepted, Refused> {
        let class = command.class();
        let outcome = match command {
            WorkerCommand::WorldLifecycle(event) => self.push_lifecycle(event),
            WorkerCommand::ConfigurationObservation(observation) => {
                let key = observation.package.clone();
                self.configuration
                    .push(&key, observation)
                    .map_err(|_| Refused::Full(class))
            }
            WorkerCommand::InputSnapshot(snapshot) => self
                .input
                .push("", snapshot)
                .map_err(|_| Refused::Full(class)),
            WorkerCommand::UiAction(action) => self
                .ui
                .push(action)
                .map(|()| Accepted::Queued)
                .map_err(|_| Refused::Full(class)),
            WorkerCommand::NetworkAuth(auth) => self
                .network
                .push("", auth)
                .map_err(|_| Refused::Full(class)),
            WorkerCommand::ReloadRequest(request) => self
                .reload
                .push("", request)
                .map_err(|_| Refused::Full(class)),
            WorkerCommand::Stop => Err(Refused::Argument),
        };
        self.count(class, outcome);
        outcome
    }

    pub(crate) fn count(&self, class: CommandClass, outcome: Result<Accepted, Refused>) {
        let index = class as usize;
        match outcome {
            Ok(Accepted::Queued) => &self.accepted[index],
            Ok(Accepted::Coalesced | Accepted::Superseded) => &self.coalesced[index],
            Err(_) => &self.refused[index],
        }
        .fetch_add(1, Ordering::Relaxed);
    }

    fn push_lifecycle(&self, event: WorldLifecycle) -> Result<Accepted, Refused> {
        let Some(slot) = self.lifecycle.get(event.world as usize) else {
            return Err(Refused::Argument);
        };
        if event.session == 0 {
            return Err(Refused::Argument);
        }
        // Newest wins: a later session, or STOPPING over STARTED of the same one.
        // One atomic max, so nothing is lost and nothing waits.
        let previous = slot.fetch_max(pack(event), Ordering::AcqRel);
        Ok(if previous == 0 {
            Accepted::Queued
        } else {
            Accepted::Superseded
        })
    }

    pub fn counters(&self) -> InboxCounters {
        let load =
            |a: &[AtomicU64; COMMAND_CLASSES]| a.each_ref().map(|c| c.load(Ordering::Relaxed));
        InboxCounters {
            accepted: load(&self.accepted),
            coalesced: load(&self.coalesced),
            refused: load(&self.refused),
            delivered: load(&self.delivered),
        }
    }

    /// The single consumer token. The first call gets it; every later call gets
    /// `None`, so two threads can never drain the same inbox.
    pub fn consumer(&self) -> Option<Consumer<'_>> {
        self.consumer_claimed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .ok()
            .map(|_| Consumer {
                inbox: self,
                live: [None; WORLD_KINDS],
                last: [0; WORLD_KINDS],
                _not_send: PhantomData,
            })
    }
}

fn pack(event: WorldLifecycle) -> u64 {
    let rank = match event.event {
        LifecycleEvent::Started => 1,
        LifecycleEvent::Stopping => 2,
    };
    (u64::from(event.session) << 2) | rank
}
fn unpack(world: u32, packed: u64) -> WorldLifecycle {
    WorldLifecycle {
        world,
        session: (packed >> 2) as u32,
        event: if packed & 3 == 2 {
            LifecycleEvent::Stopping
        } else {
            LifecycleEvent::Started
        },
    }
}

/// The worker's side of the inbox. `!Send`: it stays on the thread that claimed it.
pub struct Consumer<'a> {
    inbox: &'a Inbox,
    /// The session of each world kind this consumer last reported live.
    live: [Option<u32>; WORLD_KINDS],
    last: [u64; WORLD_KINDS],
    _not_send: PhantomData<*mut ()>,
}

impl Consumer<'_> {
    /// Takes what is pending now, in this order: world lifecycle (at most two per
    /// world: the end of the session last reported live, then the newest event),
    /// state classes (latest per key, oldest first), then the event ring in FIFO
    /// order. Each ring yields at most its capacity per call, so a producer flood
    /// cannot keep one drain going forever.
    ///
    /// A whole session that starts and stops between two drains arrives as its
    /// STOPPING alone (newer events supersede older ones), so the runtime
    /// must accept a STOPPING for a session it never saw start.
    pub fn drain(&mut self, mut sink: impl FnMut(WorkerCommand)) -> usize {
        let mut count = 0;
        let mut deliver = |command: WorkerCommand, inbox: &Inbox| {
            inbox.delivered[command.class() as usize].fetch_add(1, Ordering::Relaxed);
            count += 1;
            sink(command);
        };
        for world in 0..WORLD_KINDS {
            let packed = self.inbox.lifecycle[world].swap(0, Ordering::AcqRel);
            if packed == 0 || packed <= self.last[world] {
                continue;
            }
            self.last[world] = packed;
            let event = unpack(world as u32, packed);
            if let Some(live) = self.live[world]
                && live != event.session
            {
                deliver(
                    WorkerCommand::WorldLifecycle(WorldLifecycle {
                        world: world as u32,
                        session: live,
                        event: LifecycleEvent::Stopping,
                    }),
                    self.inbox,
                );
            }
            self.live[world] = match event.event {
                LifecycleEvent::Started => Some(event.session),
                LifecycleEvent::Stopping => None,
            };
            deliver(WorkerCommand::WorldLifecycle(event), self.inbox);
        }
        for observation in self.inbox.configuration.take() {
            deliver(
                WorkerCommand::ConfigurationObservation(observation),
                self.inbox,
            );
        }
        for snapshot in self.inbox.input.take() {
            deliver(WorkerCommand::InputSnapshot(snapshot), self.inbox);
        }
        for auth in self.inbox.network.take() {
            deliver(WorkerCommand::NetworkAuth(auth), self.inbox);
        }
        for request in self.inbox.reload.take() {
            deliver(WorkerCommand::ReloadRequest(request), self.inbox);
        }
        for _ in 0..self.inbox.ui.capacity() {
            // SAFETY: this `Consumer` is the inbox's only consumer (claimed once).
            let Some(action) = (unsafe { self.inbox.ui.pop() }) else {
                break;
            };
            deliver(WorkerCommand::UiAction(action), self.inbox);
        }
        count
    }
}

// ---- bounded MPSC ring (Vyukov's bounded queue, single consumer) ----

struct Slot<T> {
    sequence: AtomicUsize,
    value: UnsafeCell<MaybeUninit<T>>,
}

/// Lock-free for producers: a push is one CAS on `head` plus one release store,
/// and a full ring is an immediate `Err`, never a wait.
pub(crate) struct Ring<T> {
    slots: Box<[Slot<T>]>,
    mask: usize,
    head: AtomicUsize,
    tail: AtomicUsize,
}
// SAFETY: values move between threads only through the slot sequence protocol
// (release on publish, acquire on read); T itself must be Send.
unsafe impl<T: Send> Send for Ring<T> {}
unsafe impl<T: Send> Sync for Ring<T> {}

impl<T> Ring<T> {
    pub(crate) fn new(capacity: usize) -> Self {
        let capacity = capacity.max(2).next_power_of_two();
        Self {
            slots: (0..capacity)
                .map(|i| Slot {
                    sequence: AtomicUsize::new(i),
                    value: UnsafeCell::new(MaybeUninit::uninit()),
                })
                .collect(),
            mask: capacity - 1,
            head: AtomicUsize::new(0),
            tail: AtomicUsize::new(0),
        }
    }
    pub(crate) fn capacity(&self) -> usize {
        self.mask + 1
    }
    pub(crate) fn push(&self, value: T) -> Result<(), T> {
        let mut position = self.head.load(Ordering::Relaxed);
        loop {
            let slot = &self.slots[position & self.mask];
            let sequence = slot.sequence.load(Ordering::Acquire);
            let lag = sequence.wrapping_sub(position) as isize;
            if lag == 0 {
                match self.head.compare_exchange_weak(
                    position,
                    position.wrapping_add(1),
                    Ordering::Relaxed,
                    Ordering::Relaxed,
                ) {
                    Ok(_) => {
                        // SAFETY: the CAS gave this producer the slot exclusively;
                        // the consumer reads it only after the release below.
                        unsafe { (*slot.value.get()).write(value) };
                        slot.sequence
                            .store(position.wrapping_add(1), Ordering::Release);
                        return Ok(());
                    }
                    Err(current) => position = current,
                }
            } else if lag < 0 {
                // The consumer has not freed this slot yet: full. Refuse at once.
                return Err(value);
            } else {
                position = self.head.load(Ordering::Relaxed);
            }
        }
    }
    /// # Safety
    /// At most one thread calls `pop` at a time (the single consumer).
    pub(crate) unsafe fn pop(&self) -> Option<T> {
        let position = self.tail.load(Ordering::Relaxed);
        let slot = &self.slots[position & self.mask];
        if slot.sequence.load(Ordering::Acquire) != position.wrapping_add(1) {
            return None; // empty, or a producer is still writing this slot
        }
        // SAFETY: the acquire above saw the producer's release of this value.
        let value = unsafe { (*slot.value.get()).assume_init_read() };
        slot.sequence
            .store(position.wrapping_add(self.mask + 1), Ordering::Release);
        self.tail.store(position.wrapping_add(1), Ordering::Relaxed);
        Some(value)
    }
}
impl<T> Drop for Ring<T> {
    fn drop(&mut self) {
        // SAFETY: `&mut self`: no producer or consumer is left.
        while unsafe { self.pop() }.is_some() {}
    }
}

// ---- keyed latest-value slots for state classes ----

const EMPTY: u8 = 0;
const CLAIMING: u8 = 1;
const READY: u8 = 2;

struct Pending<T> {
    sequence: u64,
    value: T,
}

struct KeySlot<T> {
    state: AtomicU8,
    /// Written once by the producer that claimed the slot, before `READY`.
    key: UnsafeCell<Option<Box<str>>>,
    value: AtomicPtr<Pending<T>>,
}

/// One pending value per key. A push swaps its boxed value in and frees what it
/// swapped out, so whoever swaps a pointer out owns it: no ABA, no reclamation
/// scheme. Keys stay claimed for the inbox's life (bounded by `capacity`).
pub(crate) struct Coalescer<T> {
    slots: Box<[KeySlot<T>]>,
    sequence: AtomicU64,
}
// SAFETY: keys are immutable after the READY release; values are owned boxes
// handed over by atomic swaps.
unsafe impl<T: Send> Send for Coalescer<T> {}
unsafe impl<T: Send> Sync for Coalescer<T> {}

impl<T> Coalescer<T> {
    pub(crate) fn new(capacity: usize) -> Self {
        Self {
            slots: (0..capacity.max(1))
                .map(|_| KeySlot {
                    state: AtomicU8::new(EMPTY),
                    key: UnsafeCell::new(None),
                    value: AtomicPtr::new(std::ptr::null_mut()),
                })
                .collect(),
            sequence: AtomicU64::new(0),
        }
    }

    pub(crate) fn push(&self, key: &str, value: T) -> Result<Accepted, T> {
        let sequence = self.sequence.fetch_add(1, Ordering::Relaxed);
        let fresh = Box::into_raw(Box::new(Pending { sequence, value }));
        let count = self.slots.len();
        let start = (fnv1a(key) % count as u64) as usize;
        for probe in 0..count {
            let slot = &self.slots[(start + probe) % count];
            let mut spins = 0;
            loop {
                match slot.state.load(Ordering::Acquire) {
                    READY => {
                        // SAFETY: READY was released after the key was written,
                        // and the key never changes afterwards.
                        if unsafe { (*slot.key.get()).as_deref() } == Some(key) {
                            return Ok(Self::publish(slot, fresh));
                        }
                        break;
                    }
                    EMPTY => {
                        if slot
                            .state
                            .compare_exchange(EMPTY, CLAIMING, Ordering::Acquire, Ordering::Acquire)
                            .is_ok()
                        {
                            // SAFETY: CLAIMING is exclusive; no reader looks at
                            // the key before the READY release.
                            unsafe { *slot.key.get() = Some(key.into()) };
                            slot.state.store(READY, Ordering::Release);
                            return Ok(Self::publish(slot, fresh));
                        }
                    }
                    _ => {
                        // Another producer is writing this slot's key. Look again a
                        // few times, then probe on: a duplicate slot for one key is
                        // harmless (the drain keeps the newest), waiting is not.
                        if spins < 64 {
                            spins += 1;
                            std::hint::spin_loop();
                            continue;
                        }
                        break;
                    }
                }
            }
        }
        // SAFETY: `fresh` was never published.
        Err(unsafe { Box::from_raw(fresh) }.value)
    }

    fn publish(slot: &KeySlot<T>, fresh: *mut Pending<T>) -> Accepted {
        let older = slot.value.swap(fresh, Ordering::AcqRel);
        if older.is_null() {
            Accepted::Queued
        } else {
            // SAFETY: the swap handed this pointer to us alone.
            drop(unsafe { Box::from_raw(older) });
            Accepted::Coalesced
        }
    }

    /// Consumer side: every pending value, newest per key, oldest first.
    pub(crate) fn take(&self) -> Vec<T> {
        let mut taken: Vec<(Option<&str>, Pending<T>)> = Vec::new();
        for slot in self.slots.iter() {
            if slot.state.load(Ordering::Acquire) != READY {
                continue;
            }
            let pointer = slot.value.swap(std::ptr::null_mut(), Ordering::AcqRel);
            if !pointer.is_null() {
                // SAFETY: as in `publish`; the key is immutable once READY.
                let pending = *unsafe { Box::from_raw(pointer) };
                taken.push((unsafe { (*slot.key.get()).as_deref() }, pending));
            }
        }
        // Duplicate slots for one key (a lost claim race): keep the newest.
        taken.sort_by_key(|(_, p)| std::cmp::Reverse(p.sequence));
        let mut seen: Vec<Option<&str>> = Vec::new();
        taken.retain(|(key, _)| {
            if seen.contains(key) {
                false
            } else {
                seen.push(*key);
                true
            }
        });
        taken.reverse();
        taken.into_iter().map(|(_, p)| p.value).collect()
    }
}
impl<T> Drop for Coalescer<T> {
    fn drop(&mut self) {
        for slot in self.slots.iter_mut() {
            let pointer = *slot.value.get_mut();
            if !pointer.is_null() {
                // SAFETY: `&mut self`: nothing else can see this pointer.
                drop(unsafe { Box::from_raw(pointer) });
            }
        }
    }
}

fn fnv1a(text: &str) -> u64 {
    text.bytes().fold(0xcbf2_9ce4_8422_2325, |hash, byte| {
        (hash ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01b3)
    })
}
