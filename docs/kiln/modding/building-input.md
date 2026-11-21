# Building as the local player (BuildingInput)

`BuildingInput` (Kiln.Client, since 0.2.0-dev.30.3) sends the local player's ordinary building input from a mod: select a
building item, place, remove, undo, dismantle. Kiln writes it into the game's own input for the player, as if the
player pressed it, so the server checks permissions, resources, range and placement exactly as for the player's own
input. The native layer and its rules: [../services/client-building-input.md](../services/client-building-input.md).

> **Not checked in a real game yet.** It is a supported API on game client 26430625, covered by host tests, C# tests and
> Windows tests of the input path; the guided checks in the client pass are what a real game has not confirmed. Tell
> us what you see.

```csharp
using System.Numerics;
using Kiln;

if (!BuildingInput.Available) { Log.Info(Capabilities.Why(BuildingInput.Capability)); return; }
var r = BuildingInput.Select(roughWoodBlock, slot: 0);
if (!r.Ok) { Log.Info(r.Reason!); return; }
while (r.Request!.State is BuildingInputState.Queued or BuildingInputState.Pressed) await Client.NextTick();
r = BuildingInput.Place(at, Quaternion.Identity);
```

## What you can send

| Member | Does | Then |
|---|---|---|
| `Select(item, slot, material = default)` | picks a building item (and its terrain and building material) for an actionbar slot | dispatched at once |
| `Place(position, rotation, scale = null)` | the primary building action at a transform | pressed, released at the next input |
| `Remove(position, rotation, scale = null)` | the secondary building action at a transform | pressed, released at the next input |
| `Undo()` | the game's own building undo | pressed, released at the next input |
| `Dismantle(prop, position, rotation, scale = null)` | dismantles one exact prop | held for 1.2 s |

Each takes `expectedSession` last: 0 for the live client world, or `Client.ClientWorldSession` to refuse when the
world changed. Positions are world metres, rotations quaternions (normalized for you), each scale component in
(0, 1024]. A bad argument throws `ArgumentException` (or `ArgumentOutOfRangeException`) before anything is sent.

## Results and states

A submit answers a `BuildingInputResult`: `Ok` means queued, never "built". Read it (KILN120 warns when nobody does).
- **Refusals** (`Refusal`, with a `Reason`): `Busy` (one request slot for the whole client: every mod and kiln mode
  share it, at most 5 s), `Stale` (no client world, or another session), `NoLocalPlayer` (not spawned yet),
  `ModUnloaded`, `Rejected` (a native rule the C# checks let through).
- **Not available** throws `KilnUnavailableException` naming `client.building_input`: a dedicated server, another game
  build, safe idle, or (rarely) the first request's hook could not go in. Ask `BuildingInput.Available` first.
- **`Request.State`:** `Queued` -> `Pressed` -> `Dispatched` (sent as input; the server still decides), or `TimedOut`
  (not taken within 5 s), `Cancelled` (`Request.Cancel()`, your mod unloaded, the client world stopped). `Unknown`
  for any request but your mod's latest.

## Rules worth knowing

- **Game client only, the local player only.** A mod cannot drive another player's input. On a joined server it is your
  client's input that the server receives.
- **Your own input first.** While the player holds a building action, Kiln's request waits.
- **Any thread, never waits.** Every member may be called from any thread in mod code (OnLoad, ticks, client ticks,
  `Client.Post` work, event handlers, Kiln tasks).
- **Hooked at the first request.** Kiln hooks the game's input path only when a mod first submits, never at the start: a
  player without such a mod never has it. That first submit takes a few milliseconds more.
- **The game decides.** Building costs, permissions and the building mode are the game's: check in your mod that the
  player can build where you place.

## Testing

`Kiln.TestKit` has no game client and no input path: there `BuildingInput.Available` is false and every submit throws
`KilnUnavailableException` (reason offline), which tests your mod's fallback. In a real game, the client pass's "Building input" checks ([../client-pass.md](../client-pass.md#building-input)) walk through
select, place and remove, undo and dismantle, your own input first, and a joined client.

For kiln-mode (ShroudForge-format) mods, `runtime.world.building.submit / status / cancel` use the same input.
