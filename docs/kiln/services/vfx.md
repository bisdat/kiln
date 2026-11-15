# kiln.vfx / Kiln.Vfx: play any VFX from code

Play the game's VFX, or a mod's, at a position or attached to an entity and slot. A guard means no VFX GUID can abort the client. Forge:
`Level.addParticle`, `ParticleEngine`. Research: [docs/re/vfx.md](../../re/vfx.md).

| Layer | Where |
|---|---|
| Symbols | `sigdb/symbols/client.d/vfx.json`, features `vfx.guard`, `vfx.play` |
| C table | `native/core/abi/services/kiln_vfx.h` (`kiln.vfx` v1; since 0.2.0-dev.29 `resource_budget`, `define_vfx_result`, `vfx_registration` appended, 136 bytes) |
| Native | `native/services/vfx/` → `kiln/services/kiln-vfx.dll`. Client only. The budget: `ledger.cpp` |
| Managed | `managed/services/Kiln.Vfx/` → `kiln/host/Kiln.Vfx.dll` (namespace `Kiln`: `Vfx`, `VfxId`, `VfxHandle`, `VfxParams`, `Slot`, `Offset`) |
| Network | Kiln channel `kiln:vfx` v1, `required: false` |
| Test | `mods/tests/vfx-test`, `native/services/vfx/tests/run.sh`; the budget: `tests/test_ledger.cpp`, `managed/tests/Kiln.Contract.Tests/VfxBudgetTests.cs`, guided checks `17-vfx-budget.before` / `17-vfx-budget.after` |

## C#
```xml
<ProjectReference Include="…/managed/services/Kiln.Vfx/Kiln.Vfx.csproj" Private="false" ExcludeAssets="runtime" />
```
```csharp
using Kiln;

static readonly VfxId Boom = VfxId.Game("Explosion_Player_Spell_Fireball_1");   // a template that carries it, or a sequence that plays it
static readonly VfxId Mine = VfxId.Named("my-mod", "frost_burst");              // UUIDv5(<mod>:vfx:<name>). What Blocks.Define returns for a VfxBlock works as well

var h = Vfx.PlayAt(Boom, position);                                             // a one-shot
var aura = Vfx.PlayOn(Mine, entity, Slot.HandR, new Offset(new Vector3(0, 0.1f, 0)));   // moves with the entity, ends with it
Vfx.Set(aura, "radius", 4f);                                                    // float, int, Vector3, Color
Vfx.Stop(aura);                                                                 // Stop(aura, immediate: true) destroys it at the next update
Vfx.PlayAt(Boom, position, parameters: new VfxParams().Set("color", new Color(40, 120, 255)));
Vfx.PlayAt(Boom, position, lifetime: Vfx.UntilStopped);                         // a loop: runs until Stop
Vfx.Preload(Boom);                                                              // its first real play won't start late
```
- **Names.** VFX have none of their own. `VfxId.Game` takes a template that carries one (`VfxComponentResource.vfx`), a sequence that plays one (its first
  `VfxNotifierEvent`), or a GUID's text.
- **Client:** any thread. The call returns a handle at once, and the game is called inside its next FxController update. With no world there is no controller:
  the request waits 10 s, then fails.
- **Server** (a dedicated server, or a hosting client's server-world tick thread): `PlayAt`, `PlayOn`, `PlayFor`, `Stop` and `Set` relay to Kiln clients
  within `radius` metres (0 = 120, negative = all), and a host's own client plays them too, once: it drops the copy the relay sends its own player. The
  handle is remote: it can be stopped and set, not observed.
  Vanilla clients see nothing, and a joiner doesn't see what started before. For what every player must see, start the effect from data: an entity, a
  sequence or a buff.
- **Entity ids.** On a joined client, ids aren't proven to match the server's. `PlayOn` sends id and position, and the client attaches only if its entity of that id
  stands within 10 m of the position, otherwise plays at the position.

## Parameters
Parameters given to a play go into the block the VFX starts with, over its defaults, as the game's own templates and sequences pass them, so a burst
that spawns at once has them. The play line then ends in `N parameter(s) in its start block`. A name or type the VFX doesn't describe goes to the
game's setter once there is an instance, and the game ignores it. `Vfx.Set` on a running VFX also uses the setter: only what spawns or is read
after it changes.

A Kiln VFX (a `VfxBlock`) describes `color`, `size`, `speed`, `brightness` and `count`, all factors on what its block says
([vfx-cook.md](vfx-cook.md#per-play-parameters)). Colour, size and brightness are read at every draw, so `Vfx.Set` changes them on what is alive;
count and speed apply to what spawns afterwards.

## How long an effect runs
The game reports no "finished", so Kiln stops a one-shot itself. A [VFX block](vfx-cook.md) stops after its delay, duration and longest particle life. Any other
VFX, and a block that runs until stopped, stops after the length its resource gives: the larger of `killTimeInSeconds` and its `duration` + `lifetime`
parameters, 10 s if it says nothing, 1 to 60 s. Pass `lifetime` to say otherwise, or `Vfx.UntilStopped` for a loop.

After a stop an instance stays for its kill time at most (10 s for almost every game VFX). Measured, it goes well before that, and a stop doesn't remove what's
alive at once: use `immediate` for that. `PlayOn` runs until `Stop` or until the entity is gone. An effect that gets no instance within 5 s (a slot this body
doesn't have, a full pool) ends with `VfxEndReason.NoInstance`.

## The guard
`VfxResolve` aborts the process on a GUID the VFX system didn't register. Kiln checks three times:

| Where | For | On an unknown GUID |
|---|---|---|
| Kiln's queue | `Vfx.*` | `vfx: #N unknown VFX <guid> ignored`, the game isn't called |
| `FxStart` | everything through the FxController: templates, sequences, buffs | the GUID is emptied, and the game's own path ends the entry |
| `VfxCreateInstance` | the callers without a controller | handle 0, the path an empty instance pool takes |

The two hooks log `vfx: unknown VFX <guid> ignored (guard at …; the game would have aborted)` once per GUID and place, and count every refusal
in `Vfx.Status().Refused`. Emptying at `FxStart` matters: a start that gets handle 0 retries every frame for ever, holding a pool slot that sounds share.

At the first update the guard checks that the map reads as the signatures promise (the occupancy bits add up to the count). If it doesn't, or a read faults, the
guard turns off and the game runs as vanilla, and kiln.vfx plays nothing, since it can't tell a safe GUID from an unsafe one.

## Fail soft
| Case | What happens |
|---|---|
| Guard signatures missing on this build | `vfx: the guard's signatures did not resolve on this game build (sigdb client.d/vfx.json): kiln.vfx is off, mods play nothing`. Nothing hooked. `Vfx.Available` false, calls return `VfxHandle.None`, one warning |
| Play signatures missing | The guard runs: `vfx: feature vfx.play is off on this game build …: the guard runs, mods play nothing` |
| A game call faults | `vfx: … faulted: kiln.vfx plays nothing more this session (the guard stays on)` |
| `kiln/config/vfx.json` `{"enabled": false}` | `vfx: off by kiln/config/vfx.json: nothing hooked, mods play nothing` |
| Dedicated server | No `kiln.vfx`; calls relay |

## The resource budget (since 0.2.0-dev.29)
The game's VFX system holds a fixed number of VFX resources: its count limit, 2,048 on the game client 26430625, of which the
game's own 1,575 leave 473 for all mods together. One more than the limit aborts the game, so Kiln's builder refuses a mod that
would not fit, at the start ([vfx-cook.md](vfx-cook.md)). A mod can read what is left and define a VFX with an answer:

```csharp
var budget = Vfx.ResourceBudget;                     // client, any thread: a CapacityQueryResult
if (budget.Ok) Log.Info($"{budget.Budget.Available} VFX left of {budget.Budget.Capacity}");

var r = Blocks.Define("frost_burst", new VfxBlock { Particles = [ /* ... */ ] },
    new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));   // client, tick thread (OnLoad)
// r.State: Registered (in the game now, 1 slot), AcceptedNextStart (stored, there from the next start, 0 slots) or Refused
// (r.Code and r.Reason say why). Blocks.GetVfxRegistration(r.RegistrationId) reads the answer again later.
```

- **The budget.** `Capacity` is the count limit kiln.vfx reads from the game's VFX table (`sys + caps/vfx.map +
  caps/vfx.map_limit_relative`, sigdb `client.d/dev29-17a-vfx-cap.json`), never a number of Kiln's. `GameUsed` is the game's own
  VFX (counted in its archive at the start), `KilnUsed` Kiln's VFX of loaded mods that the table holds with the bytes Kiln
  served, `Reserved` the rest of the table: Kiln's VFX of a mod that is no longer loaded (they stay until the process ends; back
  to `KilnUsed` when the mod loads again) and other loaders' resources (the table's count less the game's and Kiln's). In-flight definition writes also reserve their
  new GUID's place in `Reserved`; a failed write or unload releases it. The file I/O holds no snapshot lock, so observation,
  budget reads and world stop can continue. NotReady replies retain the shared zero-count contract. The
  table is read on the FxController update (the thread the game makes its own VFX calls on), at the first update of a client
  world and every 30 updates after; a read of the budget copies that snapshot, never the game's memory. Before the first
  reading (the main menu has no FxController) it is `NotReady` with no counts; `UnknownExternalUsage` when the game's count
  could not be read from its archive; `AccountingMismatch` when the counts do not add up or a Kiln resource cannot be verified. The record walk checks the
  map value against the resource-table capacity and rechecks the table descriptor before copying. A served-size above the
  fixed 1 MiB sanity bound is not copied; this bound is not a measurement of a game allocation's length.
  Nothing here changes the game: a larger table (4,096) is chapter 17.1's later slice, not this release.
- **Defining with an answer.** The game lists its VFX once, before any mod loads. So a VFX is `Registered` in this process only
  when this start built that block with that content (from the mod's blocks files, its baked file, or a definition stored at an
  earlier start) and kiln.vfx found it in the game's table, by a read of the GUID map (never `vfx/Resolve`, which would acquire
  the resource), with the bytes it served, while the system's state is 2 (ready).
  `RequireRegistered`: otherwise refused, nothing stored: `RegistryClosed` (a new VFX, or a changed one), `NotReady` (built at this
  start, the table not read yet), `GameRefused` (not in the game's table, or other bytes under its GUID), `CapacityExceeded`,
  `OwnerInactive`, `IdCollision`. `AllowNextStart`: when only the moment stands in the way, the block is written whole (a temporary
  file and a rename) to `kiln/data/vfx/pending/<mod>/<name>.json` with its registration id, and the next start builds it before the
  game lists its VFX, checking it against the limit again: `AcceptedNextStart`, 0 slots. A new VFX that would not fit (what the
  table holds now, the new VFX accepted in this process, and this one, over the capacity; before the first reading, the game's own
  and the start's built VFX against the limit the game's code gives) is `CapacityExceeded` and never stored. The pending folder of
  a mod is read only for mods in the load order, so a stored definition of a mod that is gone or switched off is never built.
  VFX replay inspects at most 1,892 directory entries and 473 JSON files across the pass, with 64 KiB per file and
  473 × 64 KiB in total. Encountered excess files are named once in kiln.log and left unread; links/reparse points are refused
  using the cooker's final-handle containment checks. Both definition paths refuse to persist a larger pending file. The
  shared writer's stronger containment and durability work is separate; these bounds do not claim power-loss durability.
- **Receipts.** `RegistrationId` is UUIDv5 over the mod, the kind, the VFX's GUID and the SHA-256 of its definition (keys sorted).
  The same definition again answers the same and writes nothing. `Blocks.GetVfxRegistration(id)` says what became of it since:
  registered once the table was read, `Retained` with `OwnerInactive` for a mod that is no longer loaded (its slot still taken),
  `Superseded` when a later definition of the block replaced the stored one, `NotFound` for an id this process does not know. A
  definition stored at an earlier start is known again at this one, from its file, and says what this start made of it.
- **Events.** `CapacityChangedEvent` (`Quota` `vfx.budget`) and `DefinitionRegistrationChangedEvent`, posted at a later step and
  only while something listens (the receipts a mod's calls of this process made are read again every 25 steps when the budget's
  revision rose).
- **Capabilities.** `vfx.budget` and `blocks.vfx_registration`, reported by kiln.vfx with its table: available when this build
  can read the count limit (and, for the second, compare a resource's bytes); `symbol-missing` when the sigdb constants did not
  resolve; `faulted` when the guard turned off or Kiln's VFX are not built this session (no kiln.assets, the particle layouts
  changed). Without kiln.vfx (a dedicated server has no VFX) the rows read `service-missing`, and a member throws
  `KilnUnavailableException` naming its row; an older kiln.vfx: `service-older`.
- **Not run in a game client.** The ledger runs in host tests and on a simulated game; no real VFX table was read with it. The
  fixture is `vfx-test`. Run `17-vfx-budget.before` at the first start: it requires the exact red/green/blue definitions still
  AcceptedNextStart with zero slots and the fourth definition refused. It records process/receipt identities and the baseline
  Kiln count, then ends as Skip, never Pass. Quit to the desktop and restart with the same profile and fixture. Run
  `17-vfx-budget.after`: missing first-part evidence cannot run the proof; the same process, changed fixture receipts or a count
  other than exactly three more Kiln slots fails it. Each exact definition must now be Registered with one slot, the player
  must judge all three coloured bursts, and the budget must stay unchanged after playing. Both parts have separate catalogue
  entries, so an earlier single-part Pass cannot complete this proof.

## Threads
`FxUpdate` runs on the client world's thread (measured). The queue runs inside it, with the access rights the game's own FX calls have. Nothing in the VFX path takes a
lock, so a call from anywhere else could corrupt its free lists. Mods never call the game directly.

## Testing
No client needed:
```sh
native/services/vfx/tests/run.sh [real-guids.txt [real-resources.txt]]   # the queue, the guard's logic, on a host compiler
python3 tools/sigport/sigdb_check.py sigdb/symbols/client.json <enshrouded.exe>
# A lab server: relay and the service being absent
mods/tests/vfx-test/fixtures/relay_client.py --port <labPort> --lab <lab dir>   # compares every kiln:vfx message byte by byte
# The same lab with kiln/config/vfx.json {"sim": true}: the whole path from Kiln.Vfx to a simulated game
# {"testFailSoft": true}: the real game layer on an exe where no signature resolves
```

### What else the game draws
`kiln/config/vfx.json` `{"trace": true}` (or a number of seconds, default 3) logs every VFX instance the game creates within that time of a play
from Kiln, at most 400 lines:
`vfx: trace: the game created <guid>, the VFX's own defaults, 12 ms after Kiln's last play, from +1f3c9f5 +2a1c9f …`. `Kiln` or `the game` says whose play
it was, a parameter block shows its first 16 bytes, and the addresses are the game's code that asked for it. For telling an effect of a mod from
something the game draws next to it. Remove the setting afterwards.

### Manual test
`mods/tests/vfx-test` on a client; lines `vfx-test: PASS|FAIL` in `kiln.log`. Each case is on a key and on `/vfxtest <case>`:

| Key | Case | You should see | `kiln.log` |
|---|---|---|---|
| Ctrl+1 | A game VFX 5 m ahead | The explosion | `vfx: #N play … at … -> handle 0x…, instance 0x…`, `PASS case 1: it ended by itself` |
| Ctrl+2 | A GUID nobody registered, through Kiln's check and both guards | Nothing; the game keeps running | `PASS case 2: …` three times, `INFO case 2 done` |
| Ctrl+3 | A flame on the right hand for 5 s | It walks with you, then stops | `… on entity 1 slot 8 …`, `PASS case 3: attached` |
| Ctrl+Shift+3 | Parameters, then a loop stopped immediately | A blue explosion; 3 s later a flame for 3 s, gone at once | `PASS case 3b: …` |
| Ctrl+4 | An effect built from `blocks/vfx.json` | Red squares fly up and fade | `PASS case 4: red_burst plays` |
| Ctrl+Shift+4 | The same effect on a game module | Tells a data fault from a shader fault when Ctrl+4 shows nothing | `PASS case 4b: …` |

## The contract (dev.27)

Kiln.Vfx follows [the API contract](../modding/contract.md) as far as it applies: its play calls never throw for a missing capability
(they return `VfxHandle.None`). Since 0.2.0-dev.29 `Vfx.ResourceBudget` carries `[RequiresCapability("vfx.budget")]`,
`[ClientOnly]` and `[AnyThread]` (above).

- **Capabilities.** In the game client: `kiln.vfx` and `vfx.play` (and `kiln.screen` for screen effects). On the server
  side: `vfx.relay` (managed, session, refreshed every 25 steps) says what `Vfx.Available` says there: available while
  a Kiln client with the `kiln:vfx` channel is connected (or this host's own client can play), pending while nobody
  listens, unavailable without the channel (`needs kiln.net`), `wrong-side` in a client that only joined.
  `screen.relay` is the same for `ScreenEffects.Available`, where an open `kiln:screen` channel is enough.
- **Threads.** `State`, `IsPlaying`, `Info`, `Status`, `StatusJson` and `ScreenEffects.IsPlaying` / `Status` are
  `[AnyThread]`. The play, stop and set calls carry no thread attribute: any thread in the game client, the tick
  thread on the server side (the relay).
