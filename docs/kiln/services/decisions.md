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
  (or, for death, the first native guard) and stay for the process. If a hook cannot go in at that moment, its row is
  lowered with the reason and that `Events.On` throws `KilnUnavailableException` naming the row. While nothing is
  listened for or held, kiln.decisions gets no step window, and the step does not wait for the world's jobs on its
  account.
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
| `CapabilityReleased` | the listener went away while the action was held; it was released |
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
- An allow lets the game pick up exactly this exposure. `Amount` is read only: changing it could destroy the whole drop
  after a partial grant; a partial grant changes the amount, which is a new exposure and a new decision. The game
  scales the amount it gives by the server's `resourceDropStackAmountFactor`.
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
- Not covered: equipment the game changes by itself (autoequip when an item is made or picked up, its set swap program
  called from elsewhere).

## Capabilities

| Row | Off when |
|---|---|
| `kiln.decisions`, `kiln.window_clock` | the service is not loaded, or the core has no `tick.wait_jobs` (no step window without the world's jobs done) |
| `events.death` | its symbols do not resolve on the game build, the world layout constants (`world.storage`, `storage.kind`) are unknown, kiln.combat is missing, or its hook could not go in at the first subscription; on since 0.2.0-dev.29 and not yet confirmed in a real game (`07-events.decision-rollback`) |
| `events.pickup` | as `events.death` (also not yet confirmed in a real game: `07-events.decision-rollback`) |
| `events.equip` | kiln.playerstate cannot hold an equipment input alone (an older kiln-players.dll, no hold point on the build), or kiln.decisions is not here |
| `decisions.native_guards` | the death hook is not in (unresolved, faulted) |
| `events.build.pieces` | always in 0.2.0-dev.29: player build pieces are not cancelable yet |
| `events.build.terraform` | always: managed terraform cancel is not part of V1 (`Gameplay.Protect`'s native policy still applies) |
| `events.component_net`, `events.component_writes.exact` | always in 0.2.0-dev.29: named component watches are not built yet; an every-store feed is not promised |

While a row is off, `Events.On<T>` for its event throws `KilnUnavailableException` naming it, and nothing is
subscribed. A handler for a base class is never refused. Check `Capabilities.Is` first.

## When something faults

A fault in one of kiln.decisions' step-window calls (the core then never calls it again) or inside one of its hooks
shuts the whole service down for the process, at once: both hooks pass every death and pickup to the game, every open
decision ends `CapabilityReleased` (its `DecisionCompletedEvent` still comes), and `events.death`, `events.pickup`,
`events.equip`, `decisions.native_guards` and `kiln.decisions` read unavailable (`faulted`) with the reason; kiln.log
has one line. Nothing is held across the fault: the hold deadlines advanced at the window calls that stopped. A new
subscription to the events is refused by name; equipment inputs go on with the generic `PlayerClientActionEvent` vote
alone.

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
  at a time. Setting the first guard puts the death hook in.
- `enqueue_native(owner, request, sink, user, &id)`: a native Pre adapter's decision (kind 18, kiln.items' equipment
  sets). The dispatcher decides it like any other; `sink` is called exactly once with its outcome record, on the world
  thread, outside the service's lock.
- Both were appended after `window_end`; check `header.size` before calling.
- `prepare_kind(kind, reason, size)` (appended in 0.2.0-dev.29): puts a kind's hook in now (the managed dispatcher calls
  it at a subscription); `KILN_ERR_UNAVAILABLE` with the reason when it cannot. `window_wanted` and `window_faulted`
  after it are the core's.
- `kiln.window_clock` v1 (`kiln_window_clock.h`): the step's phase (Pre 1, Preparing 2, Sim 3, AfterJobs 4, Post 5,
  Stopping 6), the world epoch and the process session. World reads are legal on the world thread in phases 1, 2, 4
  and 5 only.

## Tests

- Offline (`managed/tests/Kiln.Decisions.Tests`, the test kit's `kiln.decisions` fake, [modding/unit-tests.md](../modding/unit-tests.md)):
  the dispatcher, the three producers, the one-verdict rule of equipment inputs.
- Host tests: `native/services/decisions/tests/run.sh` (wire layout, broker, death and pickup rules),
  `native/services/players/tests/run.sh` (the selective hold), `native/core/tests/test-window-clock.sh`.
- Lab: `mods/tests/decisions-api-test`, plain and with `kiln/control/systems.parallel`.
- Guided checks (`mods/tests/client-pass/Dev28Events.cs`): `07-events.decision-rollback` (confirms `events.death`
  and `events.pickup` in a real game; they are on without it since 0.2.0-dev.29), `07-piece-rollback` (skipped: not built), `07-native-terraform`.
