# Decisions: refusing what the game is about to do (since 0.2.0-dev.29)

A mod can refuse three things the game is about to do, and is told how each one ended:

| Event | Library | What it holds | Cancel does | Capability |
|---|---|---|---|---|
| `EntityDyingEvent` | Kiln.Combat | an entity whose health reached 0, before the game's death program marks it dead | keeps it alive at `SurvivalHealth` (1 by default, clamped to its maximum) | `events.death` (on since 0.2.0-dev.29; not yet confirmed in a real game: `07-events.decision-rollback`) |
| `ItemPickingUpEvent` | Kiln.Gameplay | a walk-over pickup (an item the game gives a player standing on it; not an interaction pickup, which is `LootEvent`) | the item stays on the ground, the receiver gets nothing, later attempts are refused without asking until the exposure changes | `events.pickup` (on since 0.2.0-dev.29; not yet confirmed in a real game: `07-events.decision-rollback`) |
| `PlayerEquippingEvent` | Kiln.Items | a player's equipment input (`changeEquipmentAction`: one slot; `swapEquipmentSetAction`: a collection swap) | the game skips the input as it skips an old one; nothing in the inventory changes | `events.equip` |

Every decision ends in one `DecisionCompletedEvent` (Kiln.Api) with the same `DecisionId` as its event, as long as
its outcome reaches the dispatcher: the service keeps at most 8192 unread outcomes, and one that finds that outbox full
is lost and counted (`outcome gap` in kiln.log) instead of posted. While the outbox is full no death or pickup is held.

```csharp
Events.On<EntityDyingEvent>(e => { e.SurvivalHealth = 1; e.Cancel(); }, EventFilter.ForEntity(player));
Events.On<ItemPickingUpEvent>(e => { if (e.Item == bannedItem) e.Cancel(); }, EventFilter.ForEntity(player));
Events.On<PlayerEquippingEvent>(e => { if (e.Change is SingleEquipmentChange { Slot: EquipmentSlot.Ring0 }) e.Cancel(); });
Events.On<DecisionCompletedEvent>(e => Log.Info($"{e.Result.Id}: {e.Result.Status} {e.Result.Reason}"));
```

The sample `mods/samples/decisions` does all four.

## When the events come

The game decides these inside its simulation, on its own threads (worker threads on a dedicated server with players).
No mod code runs there: kiln.decisions holds the action, files a request, and Kiln.Api's dispatcher posts the event in
the next Pre on the tick thread. So:

- **Death and walk-over pickup** come one or two steps after the game reached them. Until the answer the game skips
  that entity's death (the entity lies at 0 health but is not dead) or refuses the pickup (the item stays). An answer
  that has not come by the window of capture + 3 lets the game go on (`TimeoutReleased`).
- **Equipment inputs** wait one step: kiln.playerstate holds the input before the game's programs run, and the event
  is posted in the next Pre, answered there, and the game runs (or skips) the input in that step.
- **Which ones are held:** those a handler's `EventFilter.ForEntity` names; a handler without a filter (also one for
  a base class such as `KilnEvent`) holds every one of its kind. Nothing is held while nobody listens.
- **Nothing is hooked until a mod listens.** The death and pickup hooks go in at the first `Events.On` for their event
  (or, for death, the first native guard) at the safe installation point below, and stay for the process. A failure
  already known at subscription time throws `KilnUnavailableException` naming the row. While nothing is
  listened for or held, kiln.decisions gets no step window, and the step does not wait for the world's jobs on its
  account.
- **Pending installation.** Subscribing to a death or pickup decision during simulation records the request and
  registers the listener without blocking. Its row reads `pending` until installation in the server world's Pre
  (or installation at once while no world runs). No decisions are captured before the hook is in. Both hooks use a
  checked installer: if any thread cannot be checked, or is paused inside the bytes being replaced, installation is
  refused before writing. The row becomes unavailable with its reason; an existing listener learns this through
  `CapabilityChangedEvent` or by polling the row, not an exception thrown later by the tick. A subscription to an
  already unavailable row still throws `KilnUnavailableException`.
- **Only the server world's.** A game client that hosts runs the death and pickup programs in its own client and
  prediction worlds too, with entity ids that overlap the server world's; a joined client has no server world. The
  hooks tell the worlds apart by the storage the program's job names (as kiln.world's program wraps do) and pass every
  other world's visit straight to the game, counted in the world's summary line in kiln.log.

## How a decision ends (`DecisionTerminal`)

`Allowed`, `Prevented` and `AlreadyAlive` are `Ok`; the rest say why the decision did not take effect (`Reason`).

| Status | When |
|---|---|
| `Allowed` | no handler canceled: the action was released to the game (which may still not carry it out) |
| `Prevented` | a handler canceled and the action did not happen |
| `AlreadyAlive` | a canceled death whose entity had positive health again before the answer (a heal in a handler wins; nothing is written) |
| `StaleEntity` | the entity was gone, or its id belonged to another life, before the answer |
| `NoPositiveRange` | a canceled death of an entity whose maximum health is not positive |
| `WriteRefused` | kiln.combat refused the health write of a canceled death, or the health stayed at 0 |
| `TimeoutReleased` | no answer in time; the action was released |
| `CapabilityReleased` | the listener or capability went away while the action was held; it was released |
| `SourceChanged` | what was held changed under the hold (another program marked the actor dead; the pickup's receiver, item or amount changed) |
| `QueueDropped` | more than 4096 decisions were open: this action was never held (fail open) |
| `WorldEnded` | the world stopped |

`DecisionResult` names the `Subject` (the dying entity, the item on the ground, the player) and the `Actor` (the
receiver of a pickup, the player of an equipment input). A death's result also has `RequestedHealth`, `HealthBefore`
and `AppliedHealth`.

## Death

- Only deaths from health: an entity whose health reached 0 or less at the game's death boundary. A script that marks
  an actor dead directly, or removes it, is not held. This is not damage cancellation: the lethal hit has already
  changed the health and shown its effects (refuse the hit itself in `EntityHurtEvent`).
- An entity that is already dead (a corpse, or the game's state 12) is never a new decision.
- Between the lethal step and the answer (one step; two at the most) the entity has health 0 but is not dead. On a
  dedicated server nothing of the game was seen to act on that 0 (lab, 0.2.0-dev.29, a Vukah with its AI running and a
  spawned character, also with the entity programs on worker threads): no dead mark, no pending death, no AI death transition
  (the enemy AI takes its Death condition from the actor's dead state, not from health), no `EntityDeathEvent`, no
  drop; then the survival health is written. Clients are sent that health 0 for the step: whether a player's own
  screen shows anything of a death for it is the guided check `07-events.decision-rollback`. Not tested: a save
  written in that step would keep the health 0, and after a reload the game would decide that death again (held again
  only while a mod listens).
- A canceled death is carried out in Pre with kiln.combat's own health setter, and told once to `CombatChangedEvent`
  listeners (origin `kiln.decisions`), never by a second write.
- Native plugins (pets, 19d) can guard an entity's death without any mod listening: `set_death_guard` below,
  capability `decisions.native_guards`.

## Walk-over pickup

- The exposure is **sampled**: before and after each step's simulation kiln.decisions reads who the drop's receiver is
  and what it holds. A canceled exposure stays refused (without asking again) until the receiver leaves the range and
  comes back, the item or its amount changes, the drop is another life, or a mod calls
  `Decisions.ReevaluatePickup(drop, receiver)` (Pre or Post; `Ok = false`, reason `NoActiveExposure`, when there was
  none). Leaving and coming back wholly between two samples is not seen.
- An allow is a one-shot permit: the game's next attempt at this exposure runs, once, if it comes by the decision's
  deadline (two steps after the first attempt; a later one asks again). If that attempt gives the receiver nothing (an
  inventory without room), the drop stays and the game's next attempt is a new decision. A decision that ended without
  an answer (a timeout, no dispatcher, a full queue) lets one attempt run the same way. `Amount` is read only: changing
  it could destroy the whole drop after a partial grant; a partial grant changes the amount, which is a new exposure
  and a new decision. The game scales the amount it gives by the server's `resourceDropStackAmountFactor`.
- Interaction pickups (the interact key) are not held here: cancel `LootEvent` for those.

## Equipment

- One verdict per input: the same input posts `PlayerClientActionEvent` (Kiln.Players) first, with the same id in its
  `DecisionId`, then `PlayerEquippingEvent`, which starts with that vote (`PriorCanceled`, `PriorCanceledBy`,
  `Canceled`). The input is refused when either canceled; a typed handler cannot undo a generic veto.
- `SingleEquipmentChange`: `Operation` (`Equip`, `Unequip`), `Slot`, and `Selection`: `InstanceEquipment` (one item
  with its own entity), `StackEquipment` (a plain stack: `Item`, `Source` slot and `ExpectedCount`, for arrows, bait,
  spell ammunition) or `EmptyEquipment`. For an unequip the selection is what is in the slot now.
- `CollectionEquipmentChange`: the game's collection swap into the active equipment or the vanity, with the collection's
  `Contents` as read in Pre. This is the game's per-slot program, not an atomic set: on allow the game checks the
  inventory again and equips slot by slot.
- `SetEquipmentChange` is the shape of a Kiln set request (kind 18, which kiln.items files through `enqueue_native`):
  one decision for the whole set. This release declares its records only; no set operation of Kiln files one yet.
- Only these inputs wait the step; a player's other client actions do not, while only a typed listener holds.
- A held input belongs to the player who sent it: if they leave (and another player gets the same entity id), or the
  world stops, before the answer is carried out, nothing is applied to anyone. An item that left the inventory while
  the input was held is described as what the slot holds now (`EmptyEquipment`); an allow gives the input back to the
  game as sent, which checks the slot again.
- Not covered: equipment the game changes by itself (autoequip when an item is made or picked up, its set swap program
  called from elsewhere).

## Capabilities

| Row | Off when |
|---|---|
| `kiln.decisions`, `kiln.window_clock` | the service is not loaded, or the core has no `tick.wait_jobs` (no step window without the world's jobs done) |
| `events.death` | its symbols do not resolve on the game build, the world layout constants (`world.storage`, `storage.kind`, `items/storage.next_id`) are unknown, kiln.combat is missing, or its hook could not go in at the first subscription; on since 0.2.0-dev.29 and not yet confirmed in a real game (`07-events.decision-rollback`) |
| `events.pickup` | as `events.death` (also not yet confirmed in a real game: `07-events.decision-rollback`) |
| `events.equip` | kiln.playerstate cannot hold an equipment input alone (an older kiln-players.dll, no hold point on the build), or kiln.decisions is not here |
| `decisions.native_guards` | the death hook cannot be prepared (unresolved, faulted), or the service stopped |
| `events.build.pieces` | always in 0.2.0-dev.29: player build pieces are not cancelable yet |
| `events.build.terraform` | always: managed terraform cancel is not part of V1 (`Gameplay.Protect`'s native policy still applies) |
| `events.component_net` | named component watches (below): off when kiln.decisions is off, the game's `GetComponent` or `WaitWorldJobs` does not resolve on the build (a sigdb feature of each executable), kiln.world is missing, or one of the service's window calls faulted (`faulted`, for the process). On since 0.2.0-dev.29; on a game client only while it hosts the world; never run in a game client or with a connected player (`07-watch.hosted`) |
| `events.component_writes.exact` | always: a feed of every component store is not promised (component watches report net changes) |

While a row is off, `Events.On<T>` for its event throws `KilnUnavailableException` naming it, and nothing is
subscribed. A handler for a base class is never refused. Check `Capabilities.Is` first.

## Named component watches (`ComponentChanges`, since 0.2.0-dev.29)

A mod names entities and components, and is told after each step what changed, with who wrote it where Kiln knows.
The service is `kiln.component_changes` in the same DLL; the managed side is Kiln.World.

```csharp
var r = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [player], ObservedComponents.Health | ObservedComponents.Inventory,
    ComponentWatchExpansion.PlayerInventoryAndItems));
if (!r.Ok) Log.Warn($"watch refused: {r.Reason}");
Events.On<ComponentValueChangedEvent>(e => Log.Info($"{e.Entity} {e.Component} {e.Origin} {e.SourceMod}"), EventFilter.ForEntity(player));
Events.On<AttributeValueChangedEvent>(e => Log.Info($"{e.Attribute}: {e.Before} -> {e.After} of {e.MaximumAfter}"));
// later: r.Watch!.Dispose();
```

- **What is watched.** Scope `Entities`: 1 to 128 roots, as the lives they are when the watch is made (an entity
  without a spawn time that is not a player in the game is refused: its id could name a later entity). Scope `Players`:
  every player in the game, as they join and leave (a player who joins is watched from its first step, without a
  change for joining). Components: any of Health, Stamina, Mana, Fuel, Inventory, SlotSelection. Expansion
  `PlayerInventoryAndItems` also watches Inventory on every 8-slot inventory linked from a root's InventorySetup
  (Virtual links followed in link order: depth 3, at most 32 inventories and 64 setups) and Fuel on the item entities in
  them, followed again every step; a cycle, a missing setup or a tree past those bounds suspends the watch instead of
  covering part of it. Subscribing to the events watches nothing: only `Watch` does, and handlers only ever see
  watched pairs. Every watch is on the server world (`ComponentWatch.World`); a client's own world is not watched.
- **When.** The core calls the service after the last Pre subscriber and after the world's jobs, before Post (the
  step's window, `kiln.window_clock`). A change is the difference between two compares: the simulation of a step and
  the Post and Pre around it, where Kiln's own tick-thread writers run (`Combat.SetHealth`, a decision's survival
  health). It is a **net** change: a value that changed and came back between two compares raises nothing. The event is
  posted in the next Pre; `Step` is the step whose compare saw it. A new watch's first baseline is read at the next
  window, so the first event can come in the step after next. Usually one event per entity and component per step,
  whatever number of watches cover it (`WatchIds`). The exception: when a watch starts covering a pair other watches
  already cover, the pair's interval is split at that window's start, so its watches keep their own baselines; a change
  before the split (in Post or Pre) and another after it (in the simulation) give two events of the same pair with the
  same `Step`, the first for the older watches only, the second for all of them, its `Before` the first one's `After`.
  `RootEntities` names the roots it belongs to, so `EventFilter.ForEntity(root)` sees its linked inventories' changes.
  `Before` and `After` are copies of at most 256 bytes; an empty one means the component was not there, **or was larger
  than 256 bytes** (such a component is never compared: one that grows past 256 bytes looks like it went).
- **Identity.** A root is a life (its id and spawn time, or a player's session): one destroyed whose id the game gives
  to a new entity suspends its watch (`SuspendedMetadata`), at the next window's start or, when it happens inside the
  simulation, before that step's compare emits anything; no event is ever reported under the old life for the new
  entity. A linked inventory or item entity has a life only Kiln keeps (a serial, and for an item entity the item in its
  slot): one that leaves every watched tree and comes back gets a new one. An item replaced by another of the same item
  under the same entity id between two compares cannot be told apart from the same item (the engine gives no allocation
  serial for it).
- **Who wrote it (`Origin`).** Kiln's own writers tell the service what they wrote, with the bytes before and after:
  tracked writes (`World.Write`, `Update`, `WriteField`, `WriteAt`: the mod's id), program wraps' entity views (the
  wrap's mod), kiln.combat's health and attribute writes (`kiln.combat`: `SetHealth`, `Heal`, god mode's restore, the
  attribute set and add of Stamina and Mana), kiln.items' give, take, recipe, move, fuel, equip and equipment-set jobs
  (`kiln.items`). `Kiln`: one source, and its writes, in the order they were made, lead from the bytes before to the
  bytes after (net attribution: it does not prove nothing else wrote and put the same bytes back). `Mixed`: there were
  Kiln writes, but from more than one source, or something else wrote too (the chain does not reach the bytes after),
  or more than 32 writes, or one was lost (`KILN_CHANGE_TOKENS_LOST`). `Unknown`: no Kiln writer reported a write.
  **`Game` is never reported:** a change without a report may be the game's, another loader's mod or a raw write
  (`World.WriteRaw`, a hook), and Kiln cannot tell them apart. `SourceMod` names the sources (a mod's id, or the Kiln
  service that wrote for a mod it does not know), several joined by `", "`; the record's field is 128 bytes, so a long
  list can be cut short: `Mixed` stays right, the list is not exhaustive. Program wraps name their writes only while
  a managed watch exists in the process; a watch made by a native mod alone gets `Unknown` for them (never `Game`).
- **Typed attributes.** `AttributeValueChangedEvent` carries the current and maximum value of Health, Stamina and Mana
  (as kiln.combat's attribute pairs read them) and Fuel (kiln.items' fuel, in the game's units, 64 per charge), read at
  the compares; only when one of them changed, and only where the build has those readers. The raw event comes either
  way.
- **Budgets.** 32 watches (suspended ones included); 4096 distinct present (entity, component) pairs over all watches
  (overlapping watches count a pair once; an absent component is not counted, but it is still read every step); 128
  roots for any one pair. They are the process's, shared by every mod: there is no per-mod share or reservation, first
  come first served (one mod can take all 32 watches or all 4096 pairs), and the earlier watch wins when they grow;
  each watch's `State` and `Pairs` show what it holds. A `Watch` that would exceed them answers `Ok = false` with the
  reason and no watch. A watch that grows past them later (a player joins, an inventory is linked, an absent component
  appears in the simulation) is suspended whole before anything of that step is emitted for it (`SuspendedBudget`),
  with `LastCompleteStep`; one whose root is gone or is another entity now is suspended the same way
  (`SuspendedMetadata`). Either raises `ComponentWatchStateChangedEvent` in the next Pre. `ComponentChanges.Resume` starts it again from scratch (the same
  lives, the budgets again; no change from while it was suspended is reported).
- **The queue.** At most two steps' changes wait for the managed consumer (which reads every Pre); a step's frame holds
  every record of that step's compare, with interval splits and presence exchanges (4096 present pairs are not a limit
  on its records). A watch that ends is taken out of what waits, even of a batch the consumer has begun to read. A
  third step's are not queued: every ready watch is suspended (`SuspendedBudget`, "the change queue is full") and the service
  counts it; nothing is silently missing from a watch that says it is ready.
- **Ends.** `Dispose` (any thread), the mod's unload (its owner) and the server world's stop end a watch
  (`Disposed`); what was not read of a world that stopped is dropped (its entity ids would mean another world's
  entities). A watch of a stopped world cannot be resumed.
- **Cost.** While no watch exists the core makes neither window call for it and no step waits for the world's jobs on
  its account; a Kiln writer's check is a few lock-free loads (the table, the service's on, stopped and watch-count
  flags) before it copies anything; nothing is allocated. With watches: the service reads every watched pair, present
  or absent, once per step after the world's jobs, and asks each root's identity at both windows; at a window's start it checks
  the roots and links and, when they changed, plans the pairs again (new pairs read once more, a pair a new watch joins
  compared once more for the split); the exceptional recount when pairs appear past 4096 walks the watches again. A
  Kiln writer copies the bytes before its write only for an entity whose component the service may watch (a lock-free
  filter of the watched pairs: an unrelated watch costs a writer one more load). The filter is 65,536 slots by a hash of
  the entity id, one component mask per slot; collisions only add yeses, so an entity nothing watches can still pay a
  copy through a collision (the host test: 4,853 yeses among 65,536 unwatched ids with 4,000 watched entities). A new
  or resumed watch, and a link that enters a watched tree, are in the filter before their first baseline; a write
  before that baseline is outside the watch's interval by design. kiln.items still walks the inventory
  tree of the holder it changes (link ids only) to ask that filter for each entity in it. Every player of a server pays
  for the watches of every mod on it: the budgets bound the pairs, not the time, and not the time handlers take. Lab
  (`component-watch-test`, dedicated server, no player, build 26430253, 2026-11-15, four runs on a shared machine under
  heavy load, so the tails are the machine's as much as the service's; the last 1024 steps of each): 32 watches holding
  4096 present pairs (4240 in all): window_begin p50 0.33-0.41 ms, p99 0.46-2.5 ms, max 0.58-12.4 ms; window_end p50
  0.68-1.24 ms, p99 1.2-5.5 ms, max 2.5-12.6 ms. 8 watches on 1221 entities, 3860 pairs, none present: begin p50
  0.53-0.71 ms, p99 0.80-5.2 ms; end p50 0.62-1.07 ms, p99 1.1-5.5 ms (an absent pair costs a read like a present one,
  and every root is asked its identity twice a step). A kiln.items give or take job's note, p50 (p99): no watch 0.1 µs
  (0.7-0.9); a watch on another entity 0.3-0.5 µs (2.3-3.4), nothing copied; a watch on the holder 0.9-1.2 µs
  (6.0-6.7), nine or ten components copied a job.

The capability is `events.component_net`; `Watch` and `Resume` throw `KilnUnavailableException` naming it while it is
off, and so does `Events.On` for the three events. The test kit fakes the service rule for rule
(`KilnHarness.ComponentChanges`, [modding/unit-tests.md](../modding/unit-tests.md)).

## A world that goes back

Decisions reads the server world's entity-allocation counter directly, independently of kiln.items and its instance
tracking. The existing sigdb constants identify the counter on both executable builds. A decrease in the same world
epoch ends open decisions `StaleEntity` and clears death holds, pickup exposures and metadata lives together, even
when the world address, entity id and SpawnTime match. This check runs before world-safe consumer dispatch/answer
calls and at window start, including in a world a client hosts. A new world epoch establishes a fresh baseline.
This is conservative detection of counter regression; no claim is made that an in-place rollback was induced in a
real hosted game. A normal world stop remains its own invalidation path.

## When something faults

A fault in a window call or hook permanently stops holding. Every decisions-owned lock shared with a hook has a finite
attempt/time budget and checks shutdown while contended. A hook that gives up passes the action to the game and
increments its bypass counter; it never unlocks another thread's mutex. Hook-side logs do not take core logging
locks. Tick API failures shut the service down rather than waiting on a poisoned lock.

When the broker lock is usable, open requests end `CapabilityReleased` through the broker and native sinks are told
outside its lock. If the broker lock was left held, the native broker is abandoned: it cannot enumerate or notify
native `enqueue_native` sinks safely, so those sinks are **not told**. Native adapters must also observe the service's
faulted row or the optional lock-free `is_stopped` member; they must not wait indefinitely for a sink after shutdown.
The managed dispatcher completes every dispatched decision it retains without a completion locally as
`CapabilityReleased`, once, and stops calling the broker. It retains at most 4096 incomplete dispatched identities;
if that fills while outcomes cannot be read, further requests stay native and fail open at their deadlines. It cannot
recover identities it never received, and older
managed layers lack this local recovery. Normal outbox-loss limits above still apply.

`events.death`, `events.pickup`, `events.equip`, `decisions.native_guards` and `kiln.decisions` become unavailable with
the fault reason. Shutdown is logged once and persists across world restarts. Equipment inputs then retain only the
generic `PlayerClientActionEvent` vote.

## Limits

4096 open decisions and 8192 kept outcomes; a full queue fails open (`QueueDropped`). 8192 rows of identity per step:
an unfiltered death listener on a world with more entities with Health than that holds nothing and says so in the log
(`MetadataBudgetExceeded`). An entity seen first in a step, or one without a usable identity (no SpawnTime and no live
player session), is not held in that step.

## Native plugins: `kiln.decisions` v1 (`native/core/abi/services/kiln_decisions.h`)

- One consumer: the managed host's dispatcher. `attach`, `interest`, the request and outcome batches (freeze, read,
  ack; a read consumes nothing), `answer`.
- `set_death_guard(owner, fn)`: `fn(epoch, entity, health)` is asked at the death boundary for every entity with health
  0 or less in the server world, before any interest, on the death program's thread (a worker on a busy server):
  return 1 to skip that entity's death in this step. Native code of a pinned module, cheap, no managed call. One guard
  at a time. Setting the first guard requests safe installation; `KILN_OK` may mean pending until Pre. Poll
  `events.death` for that pending state; no guard runs before installation.
- `enqueue_native(owner, request, sink, user, &id)`: a native Pre adapter's decision (kind 18, kiln.items' equipment
  sets). The dispatcher decides it like any other; `sink` is called exactly once with its outcome record, on the world
  thread, outside the service's lock, except after a poisoned-lock shutdown as described above.
- Both were appended after `window_end`; check `header.size` before calling.
- `prepare_kind(kind, reason, size)` (appended in 0.2.0-dev.29): puts a kind's hook in at a safe point (the managed dispatcher calls
  it at a subscription), or requests installation in the next safe Pre, returning `KILN_OK` while the row is pending;
  `KILN_ERR_UNAVAILABLE` with the reason when it cannot. `window_wanted` and `window_faulted` after it are the core's.
  The appended `is_stopped` reads shutdown without taking a broker lock. The table is 184 bytes; check `header.size`
  covers each optional member. An older native table lacks this last member: managed recovery then uses the faulted
  service row. Older managed layers can use the original prefix but cannot locally recover poisoned-lock completions.
- `kiln.component_changes` v1 (`kiln_component_changes.h`, the second table of `kiln-decisions.dll`, 144 bytes with
  `watched` appended): one consumer (`attach`), `watch` / `resume` / `status` / `remove` / `release_owner`, the change
  batches (`begin_events`, `read_events` with a stride of at least the 1808-byte record, `ack_events`), and
  `record_write`. A batch is frozen at `begin_events`, but a watch that ends (disposed, its owner
  unloaded) is taken out of it too, so it can shrink before it is read: a native consumer uses the count `read_events`
  returns in `required`, never the one `begin_events` gave; the batch's id stays valid until `ack_events`, also when
  nothing is left in it (and an emptied batch no longer holds one of the queue's two frames). A frame is the records
  of one `window_end`, including interval splits and presence exchanges: the 4096 present pairs are not a limit on
  the records of a frame. For native writers `record_write`:
  after a write of a watched component, the whole component before and after, the source's name; any thread, copied
  during the call, a bounded lock (never waits for ever: a write it cannot take is counted as lost, and the step's
  records say so: Mixed where other writes were reported, Unknown where none was). `kiln_component_write` and `kiln_component_watching` in the header are the one-line form a writer
  uses. `window_begin`, `window_end`, `window_wanted` and `window_faulted` are the core's; `counters` (appended) gives
  what the service did, with its window calls' p50 and p99; `watched(entity)` (appended) is the writers' filter, asked
  through `kiln_component_watched` before a writer copies a component.
- `kiln.window_clock` v1 (`kiln_window_clock.h`): the step's phase (Pre 1, Preparing 2, Sim 3, AfterJobs 4, Post 5,
  Stopping 6), the world epoch and the process session. World reads are legal on the world thread in phases 1, 2, 4
  and 5 only.

## Tests

- Offline (`managed/tests/Kiln.Decisions.Tests`, the test kit's `kiln.decisions` fake, [modding/unit-tests.md](../modding/unit-tests.md)):
  the dispatcher, the three producers, the one-verdict rule of equipment inputs.
- Host tests: `native/services/decisions/tests/run.sh` (wire layout, broker, death and pickup rules),
  `native/services/players/tests/run.sh` (the selective hold), `native/core/tests/test-window-clock.sh`.
- Lab: `mods/tests/decisions-api-test`, plain and with `kiln/control/systems.parallel`; component watches:
  `mods/tests/component-watch-test` (WATCH-NET-ORIGIN, WATCH-LINKS, WATCH-BUDGET, WATCH-COST), the same way.
- Component watches, host and offline: `native/services/decisions/tests/watch_test.cpp` (the rules),
  `Kiln.Decisions.Tests/ComponentChangesTests.cs` (the managed consumer on the test kit's fake, the same cases).
- Guided checks (`mods/tests/client-pass/Dev28Events.cs`): `07-events.decision-rollback` (confirms `events.death`
  and `events.pickup` in a real game; they are on without it since 0.2.0-dev.29), `07-piece-rollback` (skipped: not built), `07-native-terraform`;
  `07-watch.hosted` (`Dev29Watches.cs`: component watches in a hosted world with a real player).
