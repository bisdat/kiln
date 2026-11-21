# Network inspector (Kiln.Replication)

`Kiln.Replication`: what the server sends each player, read only. Counters per player and channel, the full component
values of one entity as each player is sent them, what the game's component registry says about a component, and a
capture of everything one player is sent for some seconds. Nothing here changes the game's traffic. C#:
`Replication` and `ReplWatch` (`namespace Kiln`). Native: `native/core/abi/services/kiln_repl.h` (`kiln.repl` v1, in
kiln-repl.dll). Engine facts: [../../re/network.md](../../re/network.md). Since 0.2.0-dev.31.

**In a mod project** list `Replication` in `KilnServices` (and `Network` when you use `NetPeer`), or reference
`managed/services/Kiln.Replication` with `Private="false" ExcludeAssets="runtime"`. The sample is
`mods/samples/ReplWatch`.


**Tier: Advanced (a diagnostic tool).** The inspector shows the game's own data as the game sends it: components by the
game's names and registry indices, verified per game build. Names and indices may change with a game update; Kiln
reads them from the running game, so nothing breaks, but a mod that stores or compares them across game builds should
not. Mods that need stable names use Kiln's typed APIs and registries. (Owner's tiers, dev.31 decision D5.)
## What the game sends

The game replicates its ECS world to each machine (a connection slot, 0..16) on two channels:

| Channel | `ReplChannel` | What travels |
|---|---|---|
| client channel | `Client` (4) | what every client keeps of the entities it can see |
| prediction channel | `Prediction` (5) | the few entities a client predicts (its own player, what it rides or holds), with components the client channel does not carry |

Per frame and per machine and channel, the game's replication jobs (on its worker threads, after the sim step) send
creates, updates (a delta against a state the client acknowledged, or against the entity's template), and destroy
lists (ids destroyed, and ids that only left the player's view). An update with nothing changed is not sent.

**Machines and peers.** A row or an event names the peer by its full id (generation | machine), as kiln.net names it:
when another player takes a connection slot, that is a new peer and its rows start over.

- A dedicated server's machine 0 is the server itself and is sent nothing.
- In a hosted world machine 0 is the host's own client: `Local = true`, `Peer` null.
- A lab's fake player ([../modding/testing.md](../modding/testing.md#fake-players-for-lab-tests)): `Fake = true`,
  `PeerId = 0x80000000 | machine`, `Peer` null, unless a kiln.net lab peer was bound to its machine (then `Peer` is
  that lab peer).

## Capabilities

| Row | Where | What |
|---|---|---|
| `repl.observe` | server, and a game client (hosted or joined; a joined client sends nothing, so its rows stay empty) | `Stats`, `Watch`, the commands, the capture |
| `repl.client` | game client | `WatchReceived`: what this client receives |
| `repl.loopback` | lab servers only (kiln.json policy `any` and kiln.net's `labPort`) | `ReplicationLab` |

Each is a sigdb feature; `kiln.repl` lowers `repl.observe` (`faulted`) when its hooks cannot go in. A row on the
wrong side reads `wrong-side`. [caps.md](caps.md#rows-added-in-dev28) has the rows.

## API

| Member | Side, thread | Capability | Errors and result |
|---|---|---|---|
| `IReadOnlyList<ReplPeerStats> Replication.Stats()` | `[ServerOnly]` `[TickThreadOnly]` | `repl.observe` | One row per peer and channel that was sent anything. The first call by a mod turns the counters on for that mod until it unloads, and returns no rows when no one held them before; they count from the first time any mod (or Kiln's own surfaces) turned them on. `KilnUnavailableException("repl.observe")` when off; `InvalidOperationException` when kiln.repl refuses (64 holders) |
| `ReplComponentInfo? Replication.Component(string name)` | `[AnyThread]` | none | The component by its short name (as Kiln's world reads use). Null for an unknown name, or before a world was seen. `ArgumentException` for null or empty |
| `ReplWatch Replication.Watch(EntityId entity, NetPeer? peer = null)` | `[ServerOnly]` `[TickThreadOnly]` | `repl.observe` | What `peer` (every peer when null) is sent of `entity`. `ArgumentException` for `EntityId.None` or a peer that is not connected; `InvalidOperationException` when 64 watches are on; `KilnUnavailableException("repl.observe")` when off or when kiln.repl could not hook the game for it |
| `ReplWatch Replication.WatchReceived(EntityId entity)` | `[ClientOnly]` `[TickThreadOnly]` (the game's main thread) | `repl.client` | What this client receives of `entity`. `ArgumentException` for `EntityId.None`; `KilnUnavailableException("repl.client")` on a server or where the receive side is not supported; `InvalidOperationException` when no watch is free |
| `IReadOnlyList<ReplPeerStats> Replication.ReceivedStats()` | `[ClientOnly]`, the game's main thread | `repl.client` | What this client received from its server, per channel (rows with `Received = true`): frames (server ticks seen), messages, bytes, creates, updates, destroy lists and ids. The first call by a mod turns these counters on for that mod until it unloads; they start over in each new world. `KilnUnavailableException("repl.client")` when off or when kiln.repl could not hook the game; `InvalidOperationException` off the main thread |
| `ReplWatch`: `Entity`, `Peer` (null: every peer, and on a client), `IsActive` (not disposed, not ended), `HasEnded` | | | |
| `event Action<ReplSent> ReplWatch.Sent` | tick thread | | a server's watch: one per message |
| `event Action<ReplReceived> ReplWatch.Received` | main thread | | a client's watch: one per message |
| `event Action<ReplWatchEnded> ReplWatch.Ended` | tick or main thread | | the last event |
| `ReplWatch.Dispose()` | any thread | | Idempotent. No event comes after the start of the next step. A mod's watches are disposed when it unloads |

All of it reads; nothing writes, so there are no change events.

## Records

`ReplPeerStats(NetPeer? Peer, ReplChannel Channel, long Frames, long Messages, long Bytes, long Refused, long Creates,
long Updates, long DestroyLists, long DestroyedIds, long Unchanged, long TooBig, long Acks, long Resets, long Dropped)`,
with `PeerId`, `Local`, `Fake`:

| Field | Counts |
|---|---|
| `Frames` | frames that sent this peer and channel anything (one batch each) |
| `Messages`, `Bytes` | messages the session took (every type) and their bytes |
| `Refused` | messages the session could not allocate; they were not sent |
| `Creates`, `Updates` | entity messages |
| `DestroyLists`, `DestroyedIds` | destroy-list messages and the ids they named (destroyed and out of view) |
| `Unchanged` | entities the game looked at for this peer and found unchanged: no message |
| `TooBig` | entities skipped because their message was over the channel's limit (the game logs "too big to fit in one message") |
| `Acks`, `Resets` | acknowledgements the peer sent back; times it asked for full resends (it lost its baselines) |
| `Dropped` | watch records Kiln dropped because its buffer was full |

`ReplComponentInfo(string Name, int Index, int Size, uint Hash, bool ReplicationBit, bool Versioned, bool CustomCodec,
bool MayBeOnClientChannel, bool MayBeOnPredictionChannel)`: the registry index messages name it by, the bytes of one
component, its hash; `ReplicationBit`: a tracked write of it queues the entity for the client channel by itself;
`Versioned`: a version byte travels with it; `CustomCodec`: the game packs it with a codec of its own, so a client
sees it rounded.

`ReplSent(NetPeer? Peer, ReplChannel Channel, ReplKind Kind, EntityId Entity, uint Tick, uint Seq, uint BaselineTick,
int PayloadBytes, bool Gap, IReadOnlyDictionary<string, ReadOnlyMemory<byte>> Components, IReadOnlySet<string>
PackedByGame)`, with `PeerId`, `Local`, `Fake`:

- `Kind`: `Create` (2), `Update` (3), or `Destroyed` (4: a destroy list named it as destroyed; no components).
- `Tick`, `Seq`: the server tick of the message and its number on that peer's channel (what the peer acknowledges).
- `BaselineTick`: an update's baseline, the tick of the state it was coded against; 0 for a create or an update against
  the template.
- `PayloadBytes`: the delta as sent, after the message header.
- `Components`: the **full** values of every component the message carried, by component name: the state the game
  coded the delta from, never the delta. A watch started mid-session shows full values at its first message.
- `PackedByGame`: the components among them with the game's own codec; the client sees them rounded.
- `Gap`: Kiln dropped records before this one (its buffer was full); an earlier message is missing.

`ReplReceived(ReplKind Kind, EntityId Entity, uint Tick, bool Gap, IReadOnlyDictionary<string, ReadOnlyMemory<byte>>
Components)`: the values the client's game decoded. The entity id is the server's (the client uses it too).

`ReplWatchEnded(EntityId Entity, string Reason)`: `"destroyed"`, `"id reused"` (the id now belongs to an entity of
another template), `"peer left"` (the one peer the watch was for), `"world stopped"`.

## Events and their order

Events come at the start of the next step: on the tick thread on a server, on the game's main thread on a client. For
one watch they come in the order the game sent the messages: per peer and channel, `Create` and `Update` events, then
`Destroyed` for each destroy list that names the entity, then `Ended` once, last. After `Ended` the watch holds
nothing; disposing it is still fine. A handler that throws is logged with the mod's name and the watch goes on.

```csharp
var watch = Replication.Watch(chest);
watch.Sent += e =>
{
    if (e.Components.TryGetValue("Inventory", out var inv))
        Log.Info($"{e.Kind} tick {e.Tick} to {e.Peer?.Name ?? $"0x{e.PeerId:x}"}: Inventory {inv.Length} B");
};
watch.Ended += e => Log.Info($"watch over: {e.Reason}");
// later (or let the unload do it)
watch.Dispose();
```

## Cost

Kiln hooks the game's send path only while something needs it: the counters' hooks (the send callback, the entity
writer, the acknowledgement and reset handlers) while any mod or Kiln's own switch holds the stats, the watch hooks
(the entity writer, the delta writer, the send callback) while a watch or a capture exists. With none of them only the
send step's own hook stays in (it notes the GameServer once per frame, since dev.28). A watch copies only its own entity's components, into a fixed buffer per peer and
channel; the hooks allocate nothing. Measured on a lab server: see the 0.2.0-dev.31 release notes.

A fault in kiln.repl's part of a hook turns that hook off for the session (the game's function still runs once, its
result untouched) and logs `repl: observer <name> faulted at <where>; disabled` once.

## Commands

Admin only (they show what other players are sent); from chat, the server console, the control folder and
Kiln.Admin's `command` op.

| Command | What |
|---|---|
| `repl` | whether `repl.observe` is on, whether Kiln's counters are on, the commands |
| `repl stats` | the first time turns Kiln's own counters on; then one line per peer and channel since they went on |
| `repl stats off` | turns Kiln's own counters off (a mod that holds them keeps them on) |
| `repl watch <entity> [peer]` | writes one line per message about the entity (to every peer, or one) for 30 s into `kiln/logs/repl-watch-<entity>-<utc>.txt`: peer, channel, kind, tick, seq, baseline, bytes, and each component as hex (`~=` for a component the game packs). kiln.log gets only a start and an end line, without player names or bytes (its tail goes into support reports) |
| `repl watch stop` | stops the watches these commands started |
| `repl capture <peer> <seconds>` | writes everything one peer is sent for 1 to 600 s into `kiln/logs/repl-<peer>-<utc>.jsonl` (at most 256 MiB; a capture ends early when the peer leaves, the world stops or its owner goes) |
| `repl component <name>` | the registry's facts about a component: index, size, hash, channels, codec |

A peer is a player's name or a peer id as `repl stats` shows it (`0x81`, or a fake's `0x80000010`).

## Kiln.Admin ops

In [../launcher/admin-protocol.md](../launcher/admin-protocol.md) §8 (`Ops.Repl.cs`):

| Op | Arguments | Result |
|---|---|---|
| `repl_stats` | off? | the `ReplPeerStats` rows as JSON (the first call turns Kiln's counters on); `off: true` turns them off |
| `repl_watch` (async) | entity, peer?, seconds? (1..30) | the `ReplSent` events of that time, then the end |
| `repl_capture` | peer, seconds | starts a capture; the file's name under `kiln/logs` |
| `repl_component` | name | the `ReplComponentInfo` |

## The Kiln menu

The Network page has a "Replication" section: one line per peer and channel, refreshed about once a second while the
page is open. Counting is off until its switch is turned on, and the switch turns it off again (Kiln's own hold; a
mod that holds the counters keeps them on).

## The capture file

`repl capture` writes JSON lines: a header with the world's component registry, one line per message the peer was
sent (its bytes as hex and, for an entity message, the full component values the server coded), and an end line.
One capture at a time; it ends early when the peer leaves or the world stops. `tools/replinspect` reads the file and
checks that every payload byte is accounted for; its README has the format:
[tools/replinspect/README.md](../../../tools/replinspect/README.md).

## Lab harness

`ReplicationLab` (same assembly; `[Experimental("KILNX001")]`, lab servers only, capability `repl.loopback`) adds fake
players whose replication the game runs as for a real one: [../modding/testing.md](../modding/testing.md#fake-players-for-lab-tests).
`Stats` and `Watch` work on them. `mods/tests/repl-test` is the service's lab test. Package 15 (the second half of
replication) builds its filters and its own messages on this service and this harness later.

## Limits

- 64 send watches at a time (all mods and the commands together), 16 receive watches on a client. An ended watch gives
  its slot back by itself.
- One capture at a time, 1 to 600 s, at most 256 MiB, into a file of its own in kiln/logs (a plain name; never a path).
- A connection slot another player takes over within one frame: that frame is counted for neither player (the change is
  seen at the next step); a watch for one peer does not get the new player's messages.
- No public way to give back a mod's hold on the counters before it unloads (Kiln's own switch and `repl stats off` have
  theirs); the cost of holding them is small (see Cost).
- Components with the game's own codec (`PackedByGame`, `CustomCodec`; `CurrentTransform` and four more) are what the
  server coded; the client holds them rounded.
- Fakes have no player entity, no session and no client world: their rows count what the game sent them, nothing
  received.
- No receive watch on the prediction channel: `WatchReceived` gives what the client channel decodes.
- The client's receive side (`WatchReceived`, `repl.client`) is not yet confirmed in a real game (check
  `11-repl.receive-watch`).
- A full buffer drops records (`Dropped`, `Gap`); the game's traffic is never changed and nothing is asked again.
