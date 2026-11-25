# kiln.client.crosshair / `Crosshair`: the game's own crosshair hit

A read-only copy of what the game client's crosshair hits: the scene trace the stock cursor and HUD use, from the local
player's camera along its forward axis. Hit or miss, the world position and surface normal, the distance, the hit entity
and the voxel material, with the client session, step, sequence and age of the sample, so a stale read is detectable.
Kiln does no raycast of its own: the sample is exactly what the game computed that step.

**Since 0.2.0-dev.31.13, mod API 0.2.22.** Asked for by XHL for Easy HUD, Vein Mining and Blueprint. Bound on game client
26430625 (sigdb feature `client.crosshair_trace`). **Not checked in a real game yet:** a read-only sigdb test on the
exe, host tests of the copy and its threading, C# tests over a stand-in table and the test kit cover it. Tell us what
you see.

| Layer | Where |
|---|---|
| C table | `native/core/abi/services/kiln_client_crosshair.h` (`kiln.client.crosshair` v1, in-core) |
| Native | `native/core/src/services/client_crosshair.cpp` (decoder, store, rules), `client_crosshair_service.cpp` (the hook, the table) |
| Managed | `managed/services/Kiln.Client/Crosshair.cs`: `Crosshair`, `CrosshairHit`, `CrosshairReason` |
| sigdb | `sigdb/symbols/client.d/client-crosshair.json` (`client.crosshair_trace`) |
| Game side | [docs/re/client.md, "The gameplay crosshair trace"](../../re/client.md#the-gameplay-crosshair-trace-kilnclientcrosshair-26430625-2026-11-25) |
| Tests | `native/core/tests/test-client-crosshair.sh` (asan, tsan), `test-caps.sh`; `tools/sigport/tests/test_client_crosshair.py`; `managed/tests/Kiln.Api.Tests/CrosshairTests.cs`, `Kiln.TestKit.Tests/CrosshairFakeTests.cs` |

## C#

```csharp
if (Crosshair.TryGet(out var hit) && hit.IsHit)        // a fresh sample of the live session, any thread
{
    if (hit.HasEntity) Log.Info($"aiming at entity {hit.Entity} {hit.Distance:F1} m away");
    else if (hit.VoxelMaterial is { } m) Log.Info($"aiming at terrain material {m} at {hit.Position}");
}

CrosshairHit? last = Crosshair.Read(out var why);       // fresh or not; throws when client.crosshair is off
if (why == CrosshairReason.Stale) Log.Info($"last target is {last!.Value.Age.TotalMilliseconds:F0} ms old");
```

- `Crosshair.TryGet(out CrosshairHit hit)`: true only for a fresh sample of the live client session (hit or miss).
  Never throws: false when the row is off, there is no sample yet, the sample is stale or the copy faulted.
- `Crosshair.Read(out CrosshairReason reason)`: the newest sample, fresh (`None`) or not (`Stale`); null with
  `NoSample`, `Busy` (a publish was in progress every time; it asks `Crosshair.BusyRetries` more times) or `Faulted`.
  Throws `KilnUnavailableException` naming `client.crosshair` when the row is off.
- `Crosshair.Available`: the row is on and kiln.client.crosshair is there. `Crosshair.FreshFor`: 250 ms.

`CrosshairHit` (values only):

| Member | Meaning |
|---|---|
| `IsHit` | the game's trace hit something within its reach |
| `Position` | `WorldPosition?`: the hit point in world metres. The game's own point: 1 cm back toward the camera, along the surface normal or the ray, where the game places things |
| `Normal` | `Vector3?`: the surface normal, unit length |
| `Distance` | `float?`: metres from the trace's start (see below) |
| `Entity` / `HasEntity` | the game's entity of the hit (a creature, a prop, a placed object); `EntityId.None` for terrain and a miss. A client-world id; on a host also the server world's |
| `VoxelMaterial` | `byte?`: the voxel material id the game reports for the hit surface (terrain and voxel building cells; some colliders carry one) |
| `GameStatus` | the game's result byte: 0 hit, 31 nothing within reach, 21 no usable camera direction |
| `Sequence` | game traces copied since the game started; never 0. The same sequence is the same trace |
| `ClientSession`, `ClientStep` | the client world session and step of the sample (`Client.ClientWorldSession`, `Client.ClientWorldSteps`) |
| `Age`, `IsFresh` | the sample's age when it was read; fresh is at most `Crosshair.FreshFor` |

A field the game did not give as a sane value is null (a NaN position, a normal that is not unit length, a squared
distance out of range). Without `IsHit` every field is null and `Entity` is none.

## What the game's trace is

- **Reach:** the held item's (`ItemInfo.equipment.maxDistance`), clamped by the game to **7 to 31 m** (no item: 7 m).
  A target farther away is a miss (`GameStatus` 31). For a longer, voxel-only segment use `Queries.RaycastVoxels`
  (authority) or `kiln.queries.ray_voxels`.
- **Start:** the game starts the ray on the camera's forward axis, moved forward by the camera's own offset (in third
  person, about where the player is), not at the eye. `Distance` is measured from there.
- **Every step:** the game's `client_cursor` program runs it once per client step while it serves the local player. It
  does not run in the main menu or on a loading screen, and the newest sample then ages: `Stale` after 250 ms.
- **Combat lock:** the trace does not know about the game's target lock. Easy HUD's "a native combat lock takes
  precedence" stays the mod's: read `PlayerState.playerStateMask` (`PlayerFlags.InTargetLockCombatMode`) and
  `NetworkSelectedTargets` as before.
- **Not the building cursor:** the building cursor (`ClientCursor.primaryTransform`, snapping, placement checks) is
  derived later from this hit and other queries. Read `ClientCursor` / `NetworkCursor.hoveredObjectId` for those.

## Threads

Any thread: the client tick (`Client.Tick`), a `Hud.Show` text function on the UI thread, a worker. A read copies the
newest sample and never waits for the game; the game never waits for a reader. The game's copy is published by a
kiln.hooks mid link in `client_cursor` (wherever the client step runs it), lock-free: one publisher at a time (a second
visit that finds one publishing skips its copy), and a reader that meets a publish gets `Busy`, never a torn sample.
`Read` asks again up to three times.

## The lazy hook

Kiln adds its observation of the game's trace at the **first read of the process**, never at boot: a game without a mod
that reads the crosshair never has the hook. That first read answers `NoSample`; samples start at the game's next cursor
step. The install is a checked one (every other thread stopped and checked to stand outside the replaced bytes); when
some thread could not be checked nothing is written, reads keep answering `NoSample` (the row stays available) and a
read at least a second later tries again, up to three times in all. Any other failure, or the third refusal, is final for
the process: the row turns `faulted` and reads throw (`Read`) or say false (`TryGet`).

## Sessions

A sample belongs to the client session it was taken in. A new session (a hosted or joined game) starts with no sample;
after the client world stops nothing is given. A guarded copy that faults stops samples for the rest of that session
(`Faulted`); the next session copies again.

## Capability row `client.crosshair`

| State | Reason | When |
|---|---|---|
| available | none | a game client with kiln.client.crosshair, the trace verified on this build: "ready ... hooked at the first read", then "hooked" |
| unavailable | `wrong-side` | a dedicated server |
| unavailable | `policy` | safe idle |
| unavailable | `service-missing` | kiln.client.crosshair did not start, or an older Kiln (Kiln.Client reports it then) |
| unavailable | `symbol-missing` | sigdb `client.crosshair_trace` did not resolve on this build (the text names it) |
| unavailable | `faulted` | the first read's install failed for good |

A manifest may list `client.crosshair` in `features`; the core decides it from sigdb before the mods start (a later
install failure shows only at run time: check `Crosshair.Available` or use `TryGet`).

## C

```c
const KilnClientCrosshairApi* ch = (const KilnClientCrosshairApi*)api->service((KilnStr){"kiln.client.crosshair", 21}, 1);
KilnCrosshairHit hit = { sizeof hit };
uint32_t reason = 0;
if (ch && ch->get(&hit, &reason) == KILN_OK && reason == KILN_CROSSHAIR_REASON_NONE && (hit.flags & KILN_CROSSHAIR_HIT)) {
    if (hit.flags & KILN_CROSSHAIR_ENTITY) { /* hit.entity */ }
    if (hit.flags & KILN_CROSSHAIR_MATERIAL) { /* hit.material */ }
}
```

`get(KilnCrosshairHit* out, uint32_t* out_reason)`: `KILN_OK` with `*out_reason` `NONE`, `NO_SAMPLE`, `BUSY`, `STALE`
(`out` holds the newest sample) or `FAULT`; `KILN_ERR_INVALID_ARGUMENT` for a short `out`; `KILN_ERR_UNAVAILABLE` while
the row is off. `KilnCrosshairHit` is 88 bytes: `flags` (`KILN_CROSSHAIR_HIT`, `_POSITION`, `_NORMAL`, `_DISTANCE`,
`_ENTITY`, `_MATERIAL`), `status`, `entity`, `material`, `client_session`, `sequence`, `client_step`, `age_ns`,
`position[3]` (double), `normal[3]`, `distance`.

## Example: an Easy HUD-like readout and a Vein Mining-like check

The two things XHL's Easy HUD and Vein Mining do with the game's hit: a label under the crosshair for what the player
looks at, and "am I aiming at ore with my pickaxe" before doing mining work. Both read the same sample; neither raycasts.

```csharp
// SPDX-License-Identifier: MIT
using Kiln;

public sealed class AimReadout : Mod
{
    private HudLine? _line;

    public override void OnLoad()
    {
        if (!Client.IsClient) return;
        // Easy HUD-like: the text function runs on the UI thread every frame; reading the crosshair there is fine.
        _line = Hud.Show("aimreadout.target", Describe, HudAnchor.TopCenter, Color.White);
        Client.Tick += OnClientTick;
    }

    public override void OnUnload()
    {
        Client.Tick -= OnClientTick;
        _line?.Dispose();
    }

    internal static string Describe()
    {
        if (!Crosshair.TryGet(out var hit)) return "";                 // no fresh sample: draw nothing
        if (!hit.IsHit) return "";                                     // nothing within the held item's reach
        var distance = hit.Distance is { } d ? $" · {d:F1} m" : "";
        if (hit.HasEntity) return $"entity {hit.Entity}{distance}";    // look its name up by template, NPC id, ...
        if (hit.VoxelMaterial is { } m) return $"terrain {m}{distance}";
        return $"surface{distance}";
    }

    // Vein Mining-like: on the client tick, is the player aiming at an ore vein within reach?
    private static readonly HashSet<byte> Ores = [37, 38, 41];         // the mod's own material ids (illustrative)
    private ulong _lastSequence;

    private void OnClientTick(in TickInfo tick)
    {
        if (!Crosshair.TryGet(out var hit) || hit.Sequence == _lastSequence) return;  // a new trace only
        _lastSequence = hit.Sequence;
        var aimingAtOre = hit.IsHit && !hit.HasEntity && hit.VoxelMaterial is { } m && Ores.Contains(m)
                          && hit.Distance is <= 10f;
        if (aimingAtOre)
        {
            // e.g. highlight the vein or prepare the vein for the next strike. The world edit itself stays on the
            // authority (Build.Dig, Terrain.Apply); the client hit only says what the player looks at.
        }
    }
}
```

Unit tests for it run on the test kit's fake: `kiln.Crosshair.Aim(new WorldPosition(1, 2, 3), voxelMaterial: 37,
distance: 4f)` makes the next read a fresh terrain hit; `AimAtNothing`, `Age(TimeSpan)`, `Fault()` and `Clear()` script
the other answers ([modding/unit-tests.md](../modding/unit-tests.md#recording-fakes-since-020-dev27), "Crosshair").
