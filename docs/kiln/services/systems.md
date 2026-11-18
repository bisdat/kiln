# Kiln.Systems: wraps on the game's ECS programs

Your code before, after or instead of one of the game's own ECS programs (`fuel_setup`, `health_recharge`, ...), per
slice or per entity, in the program's own slot of the scheduler and on its thread. Managed: `Kiln.Systems`
(`KilnServices` `Systems`); native: kiln.world v4 (`native/core/abi/services/kiln_world.h`, "game programs"). Server
world. Since 0.2.0-dev.29.

| Capability | What it says |
|---|---|
| `systems.wraps` | wraps work on this build; unavailable while native SET is loaded (reason `policy`) |
| `systems.wraps.typed` | typed wraps (`Systems.Wrap<T>` with a view of Kiln.Game's `Ecs.Programs`); follows `systems.wraps` (each wrap checks its view's contract against the live descriptor) |
| `systems.wraps.client` | wraps on the client world's programs (`WrapOptions.World = WorldKind.Client`), in a game client; `wrong-side` on a dedicated server. On since 0.2.0-dev.29 (held in dev.28), not yet confirmed in a real game: guided check `06a-wraps.client` |
| `systems.wraps.prediction` | wraps on the programs the prediction storage runs (`WrapOptions.World = WorldKind.Prediction`, since 0.2.0-dev.29.18, mod API 0.2.10): on where `systems.wraps.client` is, off where it is off, with the same reason. Not yet run in a real game ([below](#the-prediction-storage-worldkindprediction-since-020-dev2918)) |
| `systems.define` | `Systems.Define`: a program of the mod's own over a `[Query]` struct, reserved at boot (kiln.world's own programs, `world.programs`) |
| `systems.threads` | the server world's players mask can be read (what puts program slices on the game's workers) |
| `world.systems` | the sigdb feature behind them: the program scheduler and the players mask |

## Finding a program

```csharp
var info = Systems.Find("fuel_setup");        // null: not in that world (yet)
foreach (var p in Systems.All()) Log.Info($"{p.Name} writes {string.Join(",", p.Writes)}");
```

`SystemInfo` has the name, the index in that world's table now (indices move between game builds: use names), the
descriptor's flags, whether it has an update in this world (`Runs`; a client program in a server world has none),
the components it writes and the managed wraps on it. Any thread; empty before the world's first step.

## Wrapping

```csharp
var made = Systems.Wrap("fuel_setup");
if (!made.Ok) { Log.Warn(made.Reason!); return; }
var wrap = made.Wrap!;
wrap.Before((ref Slice s) => Interlocked.Add(ref seen, s.Count))
    .After((ref Slice s) => { /* after the game's update of this slice */ });
...
wrap.Dispose();   // or let the mod's unload remove it
```

- **Stages.** `Before` and `After` per slice (one archetype group's range of the program's entities), `BeforeEach`
  and `AfterEach` per entity, `Replace` instead of the update. Befores run high priority first (ties in the order
  made), Afters in the exact reverse order. A stage's `priority` defaults to `WrapOptions.Priority`.
- **Replace.** One per program, across all mods: a second one returns `Ok = false` with a reason that names the mod
  holding it. Call `Slice.RunOriginal()` to run the game's update (once). A Replace that throws before calling it gets
  the original run for it. List the program under `"systems": {"replaces": [...]}` in `kiln.mod.json`: a declaration for tools
  that look for two mods replacing one program before play (the launcher and `kiln lint` do not read it yet; nothing
  changes at run time). A replaced program emits none of the
  game's events and makes none of its structural changes unless your handler does; there is no per-entity Replace.
- **Filters.** `When(info => ...)` runs on every slice (`SliceInfo`: count, step, thread, archetype key);
  `WhenArchetype(has, lacks)` decides once per archetype and world (component names as in kiln.world). One that cannot
  decide (the build gives no archetype key, a name the world has no component of) turns its stage off with the reason
  (`Disabled`) instead of skipping every slice; an empty slice is skipped. Since 0.2.0-dev.29 kiln.world reads the
  archetype from the engine's own record of the slice, the one the game's program runner set the slice's columns up
  from. The first archetype read asks for a link around the runner, put in in a later Pre with the checked install (it
  checks the game's other threads; refused, it is tried again 16 steps later, eight times at most). The link keeps the
  slice's record for exactly that runner call: when the call returns, is skipped by another hook or unwinds, the record
  is gone, so a later slice can never read an earlier slice's archetype (kiln.log: "WhenArchetype reads each slice's
  archetype from the engine's slice record (a scoped checked link on types/ProgramRunner ...)").
  - **Server world:** until the link is in, or where it cannot be had, the archetype comes from the slice's first entity
    in the game's entity map. That read is narrowed, not proven: kiln.world compares the map's size and the entity's
    record before and after it, which catches most changes under the read but not an erase and an insert in between,
    nor a record reused for the same id; it relies on the game deferring structural changes to the step's end.
  - **Client world:** never from the entity map (the network can create and destroy entities on another thread).
    While the link is not in yet, or a slice has no record of its own, the filter leaves that slice to the game; where
    the build has no slice record or eight installs failed, the filter cannot decide and its stage goes off with the
    reason.
  - A transient answer (a change under a server map read, a client slice without its record) is read again (three reads
    in all), then that one slice is left to the game: the stage stays on, `Stats.ArchetypeSkips` counts it and kiln.log
    says so once per stage.
- **Creating and destroying entities.** Not from a handler: `Kiln.Spawn` refuses there (managed and native, since
  0.2.0-dev.29 also on the world's own thread) with a message that names `Slice.Post`; post the call and it runs before
  the next step. (A `GameSystems.Hook` callback is not a handler: there such a call still goes through, as in
  0.2.0-dev.28, with a warning.)
- **Lifetime.** `Wrap` is tick thread and returns at once; the stages apply in the target world's next step
  (`State` Pending → Active), again by program name in every later world of that kind, and go with `Dispose` (applied in
  the next step: no call starts after it) or when the mod unloads (the unload waits until no call of the mod's stages
  runs, up to 2 s; a still-running callback remains rooted and is reaped after it finishes). `Stats` sums calls, entities, time, faults and budget overruns over the wrap's stages.
- **Refusals.** `Ok = false`: the program is not in that world, it has no update there, native SET is loaded (it
  replaces the game's program runner, so Kiln stays out of it and takes existing wraps out). A capability that is off
  throws `KilnUnavailableException`. `WorldKind.Prediction` throws `ArgumentException` where `systems.wraps.prediction`
  is off (the prediction storage has no table of its own; before 0.2.0-dev.29.18 always); where it is on, see
  [the prediction storage](#the-prediction-storage-worldkindprediction-since-020-dev2918).

### The prediction storage (`WorldKind.Prediction`, since 0.2.0-dev.29.18)

A game client runs the local player's own character ahead of the server in its **prediction storage**: every
prediction step (on the main thread, more often than the client world steps), it runs the programs of the client
world's table that are not client programs and not `hostOnly` (descriptor flags without 0x1, 0x10 and the engine's
0x40), on the entities the server lets it predict, the local player's character among them. What the server computes
for that character, the player's own client computes again there, and the player's own screen follows it:
`equipment_visuals` recomputes `EquipmentAnimationSets` (the stance) from the held item there too, and
`locomotion_animation` reads it there. A server wrap that changes the stance is seen by everyone else and not by that
player, unless the same change is made in the prediction storage as well
([docs/re/world.md](../../re/world.md#the-prediction-storage-recomputes-the-stance)).

```csharp
// the server half: everyone's view of player P (server world, the default)
var server = Systems.Wrap<Ecs.Programs.EquipmentVisuals>().Wrap!;
// the client half: this player's own view, in the prediction storage (a client or both mod, on a game client)
if (Capabilities.Is(Systems.PredictionCapability))
{
    var predicted = Systems.Wrap<Ecs.Programs.EquipmentVisuals>(new WrapOptions { World = WorldKind.Prediction }).Wrap!;
    predicted.AfterEach((ref Ecs.Programs.EquipmentVisuals v) =>
    {
        if (!holstered) return;                                          // your state, shared through a volatile/atomic
        ref var sets = ref v.EquipmentAnimationSets;                     // after the game's own write of this step
        sets.primaryAnimationSet = Ecs.PrimaryAnimationSet.None;
        sets.secondaryAnimationSet = Ecs.SecondaryAnimationSet.None;
    });
}
```

- **What it is.** A wrap of a program of the **client world's table** whose stages run on the prediction storage's
  slices only (`Slice.World` and `SliceInfo.World` say `Prediction`); a client-world wrap of the same program runs on
  the client world's slices only. `Systems.Find(name, WorldKind.Client)` finds the program (the prediction storage has
  no directory of its own: `Systems.Find(name, WorldKind.Prediction)` is null, as before).
- **What it takes.** `Before`, `After`, `BeforeEach`, `AfterEach` (with an `EntityView` or a typed view) and the
  filters. No `Replace` (`Ok = false`). A program the prediction storage never steps (a client program, a `hostOnly` one,
  one with the engine's 0x40) is `Ok = false` with its flags in the reason.
- **When.** Applied in the client world's Pre, like a client wrap; again in every later client world by name (the
  prediction storage starts and stops with its client world); gone with `Dispose` or the mod's unload.
- **Threads.** The prediction storage steps serially on the main thread. `Slice.Step` is the client world's step at its
  last Pre and `IsWorldThread` compares with the client world's thread. No tick-thread API inside a stage, as anywhere.
- **Writes** go to the prediction storage only and are marked with the job's tracker, as the game's own update there
  marks them. No `ComponentWrittenEvent` (server world only). The next server state the client receives for the
  predicted entity can put the game's value back until your stage runs again; write in every step you mean it.
- **WhenArchetype** reads the slice's archetype from the engine's slice record and the prediction storage's own
  component indices (no entity-map read, as for the client world).
- Not run in a game yet. What a real client shows: the local player's stance follows a write in an After of
  `equipment_visuals` there, with no flicker against the server's state.

## Threads, budget, faults

Handlers run on the thread that runs the slice: the server world's own thread while no player is in it, the game's
task workers once one is (the client world always uses workers). So:

- No tick-thread API inside a handler: `Services.RequireTickThread` and `Game.AssertTickThread` throw there, even on the
  world's own thread. Use `Slice.Post(action)` to run work on the tick thread before the next step, in your mod.
- Keep state per slice, or share it through atomics; several slices of one program run at once.
- **Budget:** 1 ms per stage per slice, the game's own update excluded. Ten slices in a row over it turn the stage off.
- **Faults and exceptions** turn off that stage only, at once; the other stages of the wrap keep running. The wrap
  raises `Disabled` (`WrapDisabledEvent`, tick thread) with the reason, and a managed exception also counts toward the
  host's fault policy of the mod (five in 60 s disable the mod).

A view stage whose directory lists/contract could not be read within the native lock's bound is suspended (`Pending`)
with a logged reason. It retries at most once per world step, up to eight failed attempts per stage and world session.
A successful retry resumes the same stage; after eight failed steps it is removed and raises `Disabled`. These failures
do not count against the mod's fault policy. Cached successful lists and contracts are reused; failed reads are not cached.

## Per entity: reading and writing the program's components

```csharp
wrap.AfterEach((ref EntityView e) =>
{
    if (!e.Has("Fuel")) return;
    var fuel = e.Attribute("Fuel");      // Fuel: [0] current units, [1] the maximum
    fuel[0] = Math.Min(fuel[0] + 64, fuel[1]);
    fuel.Changed();                      // the game's attribute formulas (clamps, derived values)
    ref readonly var at = ref e.Read<Kiln.Game.CurrentTransform>("CurrentTransform");
});
```

`BeforeEach` / `AfterEach` with an `EntityHandler` get the entity id only; with an `EntityViewHandler` they get an
`EntityView`: the entity, its row and the step, and its components through the program's **own** slots, so only what the
program's descriptor lists (`SystemInfo.Writes` for the written ones; singletons such as `impact::AttributeRegistry`
read too):

| Call | What it does |
|---|---|
| `Has(name)` | the program lists it and the entity has it now (optional slots may be absent); never throws |
| `Read<T>(name)` | a `ref readonly` into the game's storage; `T` at most the component's reflected size (Kiln.Game's structs fit) |
| `Write<T>(name)` | a `ref` into the game's storage, write lists only. The first use per entity and component in the stage marks it for the save and replication (the game's own MarkWritten) |
| `Attribute(name)` | an `Attribute32` over a written attribute component (Health, Stamina, Mana, Fuel, Level ...): `Values`, an indexer, `RootId`, `HasValues`, and `Changed()`, which runs the game's attribute formulas with the program's own `impact::AttributeRegistry` (the program must declare it, else it throws) |

A name the program does not list, an optional component the entity lacks, or a `T` larger than the component throws
`InvalidOperationException`. Programs whose descriptor uses whole-world lists (`other_write_unser`, `all_read`,
`all_write`: kiln.world flags them `NO_VIEW`) have no slots: wraps and per-entity ids work, the accessors throw.
Which attribute slot is which is the attribute's own: Health `[0]` current, `[1]` minimum, `[7]` maximum
(docs/re/combat.md); Fuel `[0]` current, `[1]` maximum (docs/re/items.md).

### Typed views (`Systems.Wrap<T>`)

Kiln.Game carries a typed view of each of the game's programs, generated from the program catalog of the game build
it was made for (`sigdb/programs/<side>/<sha256>.json`, [typegen.md](../typegen.md) §4.5): `Kiln.Ecs.Programs.FuelSetup`
for `fuel_setup`, with a property per component its descriptor lists that the type dump has a layout for, in the
program's own slot order:

```csharp
var made = Systems.Wrap<Ecs.Programs.FuelSetup>();
if (!made.Ok) { Log.Warn(made.Reason!); return; }       // e.g. "fuel_setup: ... write list changed: +X -Y"
made.Wrap!.AroundEach(
    capture: (in Ecs.Programs.FuelSetup w) => w.FuelValues[0],               // before the game's update (not marked)
    apply: (ref Ecs.Programs.FuelSetup w, in int before) =>                  // after it, with what capture kept
    {
        var added = w.FuelValues[0] - before;
        if (added <= 0) return;
        var fuel = w.Fuel;                                                   // Attribute32: marked for the save
        fuel[0] += 3 * added;                                                // four times what the game added
        fuel.Changed();                                                      // clamps to the maximum
    });
```

| View member | What it is |
|---|---|
| `Name`, `Contract`, `Lists` (static) | the program, the FNV-1a-32 of its contract text (all 21 lists, three masks, flags, interval; chapter 06a 9.7) and that text without its empty lists |
| `Entity`, `View` | the entity, and its untyped `EntityView` (names, singletons such as `impact::AttributeRegistry`) |
| a `tpl`/`read` component `X` | `ref readonly Ecs.X X` |
| an optional one (`tpl_opt`, `read_opt`, `write_opt`) | `bool HasX` and the reference (an entity without it throws: ask first) |
| a `write` component | `ref Ecs.X X`: the first use per entity marks it for the save and replication, as `Write<T>` |
| a `write` attribute (int slots: Fuel, Health, ...) | `Attribute32 X` (marked when used) and `ReadOnlySpan<int> XValues` (read, not marked) |

- **The contract.** `Wrap<T>` asks kiln.world for the live descriptor's contract (`game_program_contract`) and refuses
  (`Ok = false`) when it is not the view's: on a game build whose program changed, the reason names each list that did
  (`+added -removed`), the flags or interval. A view binds an entity only on a slice of its own program whose live
  contract is its own (asked once per world session).
- **What each check covers.** Three checks stand between a view and the game's memory, each for one thing:
  the **contract** (the FNV-1a-32 of the program's descriptor: its 21 lists in order, the masks, flags and interval) says
  the view's positions name the program's own slots, which component sits in which slot; it says nothing about a
  component's fields. The **layout check** (Kiln.Game's `Ecs.WhyNot<T>()`, recursive: every field, nested struct and enum
  value of the component against the running game's type table, [typegen.md](../typegen.md)) runs before a typed
  property hands out a reference, so a moved field or a changed nested layout refuses the access (it throws) instead of
  reading the wrong bytes. The **reflected size** (kiln.world's slot size, `game_slice_slot`) bounds every access: a `T`
  larger than the component is refused, and a component whose size kiln.world does not know refuses typed and untyped
  access alike. `Attribute32` takes neither the layout check nor `T`: kiln.world checks that the slot's component is an
  attribute and that its value block lies inside the component (`game_slice_attr_validate`) before any byte or formula.
- **AroundEach** is a Before (capture, one `TSnap` per entity, kept for that slice on its thread) and an After (apply)
  at one priority: an onion around the higher priorities' stages. A capture that throws turns both off for good; that
  slice's snapshots are dropped and its update is the game's. Rows do not move inside a slice (structural changes wait
  for the step's end), so apply sees each entity with its own snapshot.
- **What has no typed property:** a component the type dump has no layout for (the `Dynamic*` components, such as
  `DynamicFuelSetup`: kiln.world does not know their size, so even the untyped accessors refuse them), the singleton,
  other-entity, filter and event lists. The 15 programs whose descriptor uses `other_write_unser`, `all_read` or
  `all_write` have no view at all (untyped wraps still work).
- Typed stages count, fault and budget like untyped ones; `ProgramWrap<T>.Untyped` is the wrap underneath.
- A whole mod: the sample `mods/samples/WaterWorks` (wells refill `WellRefill` times as fast) and the template
  `kiln-system-wrap` (`dotnet new kiln-system-wrap`).

### Change events

A stage's writes through a view are compared when the stage is done with the entity. Changed components are coalesced
per step per entity and component (the bytes before the first stage that wrote, after the last one, every writer
once), and at the next step's start Kiln raises `ComponentWrittenEvent` on the tick thread, once each, with
`Writer = "<modid>/wrap:<program>"` (`"my-mod/wrap:fuel_setup"`) and `Writers` listing every wrap that wrote it in that
step. `Result.Before` and `Result.After` span everything between those two stages, the game's own update included when
a Before and an After both wrote. Nothing is copied or compared while nobody listens for the event. A store through a
pointer kept past the call is neither marked nor raised. Server world only.

## Programs of your own: `Systems.Define`

A program the game does not have, over the entities a `[Query]` struct describes, scheduled by the game like its own:
once per matching entity of every step, on the game's workers, after every game program it conflicts with.

```csharp
[Query(Template = ["FuelSetup"], Write = ["Fuel"], HostOnly = true)]   // the access lists, the game's names
public ref struct Containers : IQuery<Containers>
{
    public EcsProgramContext Context;                                  // what the lists allow, for this entity
    public static Containers Bind(EcsProgramContext context) => new() { Context = context };
}

public override void OnLoad()
{
    var r = Systems.Define("MyMod_Containers", new ProgramOptions { HostOnly = true }, (ref Containers c) =>
    {
        if (c.Context.TryGet<Ecs.Fuel>(out var fuel) && fuel.Values[0] < fuel.Values[1]) { /* ... */ }
    });
    if (!r.Ok) Log.Warn(r.Reason!);   // "not reserved at boot: rebuild the mod and restart the server"
}
```

- **Reserved at boot.** The game registers its programs once, before the world starts, so a program of a mod must be
  known before any managed code runs. The build does it: Kiln.Sdk's `KilnPrograms` step writes each `[Query]` struct of
  the mod into the mod's `kiln.programs.json` (key: the attribute's `Key`, or the struct's full name with `.` and `+` as
  `_`; the lists; the flags), and kiln.world reserves those programs when the server starts. `Define` binds the handler
  to the reservation by that key. Entries the project's own `kiln.programs.json` declares stay; a hand-written entry
  under a `[Query]`'s key with other lists, more than 32 programs (the boot limit per file), a bad name or a query without an entity list stops the
  build (KILN036). A mod built without the Kiln SDK writes the file itself in the same form
  (`schema/kiln-programs.schema.json`).
- **Results.** `ProgramRegistration`: `Ok = false` with `"not reserved at boot: rebuild the mod and restart the server"`
  when kiln.world reserved nothing under the key for the mod (the file is missing, or the mod was loaded after the server
  started), or with kiln.world's refusal (another binding holds it). `ArgumentException`: the struct has no `[Query]`,
  the key or the options are not the `[Query]`'s. Before publishing a callback, Define reads the immutable declaration
  reserved at boot for `(mod id, key)` through `kiln.world.budget.program_reservation`. All 16 lists (including their
  name order, required/optional split, and With/Without/AnyOf), flags and interval must match exactly. Query programs
  currently reserve interval 1. A difference returns `Ok = false` naming the first field and saying
  "rebuild is in the game only after a server restart: this reservation was made at boot from the earlier declaration".
  Reloading an unchanged mod can bind again after disposal; reloading a changed declaration cannot change the snapshot.
  A core without the reservation-query member throws `KilnUnavailableException("systems.define")`; it never binds unchecked.
- **The handler** gets the query for one entity; the query reads and writes through `EcsProgramContext` (`TryGet`,
  `TrySet`, `TryReadOther`, events, `QueueAdd`), as `EcsPrograms.Bind` does. Worker threads: no tick-thread API inside
  it; a handler that throws is turned off with a line in the mod's log. `Dispose` (tick thread) unbinds it and waits
  until no call runs; the mod's unload does too. The reservation stays, inert, until the server restarts, and a reload
  of the mod binds it again.
- `[ServerOnly]` (the server world, also on a host). Lab: `mods/tests/systems-test` binds one in `OnLoad`, checks it ran
  for `fuel_setup`'s containers, that a query without a reservation is `Ok = false`, and its handler on N > 1 threads with
  the lab parallel switch. Those rows do not establish changed-declaration refusal across reload, blocked-worker unload,
  or hosted/client gameplay. These are requirements in the lab source, not a lab run made by this repair. Host tests
  cover declaration equality; the changed-query reload row still needs a lab run.

## Unit tests

Under `dotnet test` with Kiln.TestKit the wraps run on the kit's fake of kiln.world v4: the test declares the programs
(`kiln.Programs.Add`, or `Add<Ecs.Programs.FuelSetup>()` for a typed view) and each step runs them over the fake world's
entities through the mod's stages, typed views and `AroundEach` included; marks and `ComponentWrittenEvent` follow.
The fake models the 1 ms stage budget (excluding the declared original), ten consecutive overruns disabling the stage,
and the real managed exception/mod fault policy. Not modelled: worker threads (one thread, the harness's tick thread), singleton slots, the attribute formulas,
native faults, client worlds and `Systems.Define` (offline). The details:
[modding/unit-tests.md](../modding/unit-tests.md#program-wraps).

## In the lab: the parallel switch

On a dedicated server with no player every slice runs on the world's thread. `Kiln.Testing.LabSystems`
(`KILNX001`, needs `kiln/control/systems.parallel`) forces the game's own worker split for up to 3000 steps, so a test
can run its handlers on N > 1 threads: `LabSystems.ForceParallel(steps)`, `StopParallel()`, `Forced`, `PlayersMask`.
`mods/tests/systems-test` covers Before/After, iteration and slots in serial and forced-worker runs. Replace and INIT
were tested serial only; the managed write-event row used forced workers. The reload sequence does not prove an unload
interleaved with a blocked worker callback. Since 0.2.0-dev.29 it also checks every live program's contract against the
committed catalog (817 of 817 on the lab's server build), a typed view's slots against kiln.world's named ones and
GetComponent, a doctored contract's refusal, `AroundEach` on `fuel_setup` with forced workers, and a wrap applied when
the world stops (the drain, and the wrap back to Pending). A dedicated server cannot stop and start its world in one
process: the world-session keys of the caches are proven by the unit tests, the next boot only re-applies by name.

## Native: kiln.world v4

The C table appends the game-program members to kiln.world (`KILN_WORLD_VERSION 4`): `game_program_find`, `_info`,
`_list`, `game_programs_status`, `game_program_wrap` / `_unwrap`, `game_wrap_info`, `game_wraps_release`,
`game_slice_run_original`, `wrap_original`, and per entity inside a stage `game_slice_iterate`, `game_slice_slot`,
`game_slice_mark`, `game_slice_attr_validate`, `game_slice_attr_changed`, `game_slice_archetype`, `game_archetype_has`;
`game_world_session` (a lifecycle generation per world kind) and `game_program_contract` (a descriptor's contract and
its text, since 0.2.0-dev.29). The header documents each
(threads, results); `mods/tests/systems-test/native` uses all of them. A native stage's fault (SEH) is caught and turns
that stage off; `KilnWrapStageFn` runs on the slice's thread.

Native callback lifetime: call `game_wraps_release` and poll until `out_pending == 0` before freeing anything a stage
uses (callback code, user data or other referenced state). Do this outside every stage. Owner retirement prevents new
calls from entering; it does not wait for an already-running callback. The core retaining a plugin module does not
keep memory the plugin frees itself alive.

## Limits

- One Replace per program; no per-entity Replace.
- The server world by default. In a game client, `WrapOptions.World = WorldKind.Client` wraps the client world's
  programs (on since 0.2.0-dev.29); the client world always runs its programs as tasks on the game's workers. The
  prediction world has no table of its own: it runs the client world's, and its slices reach the game's update untouched.
  A wrap belongs to one world: it is applied in that world's own step, taken out when that world stops, and applied again
  by name in the next world of that kind.
- A world's stop: kiln.world closes the stages' admission and waits up to 2 s for a call in flight. Which guarantee a stop
  has is the core's capability row `world.stop_drain.server`, `.client` or `.prediction` (`systems.wraps` and
  `systems.wraps.client` name it, and so does kiln.log at each stop):
  - **Drained by the engine** (the row is available; the supported builds, with a working tick): the game itself lets no
    program slice outlive the step that ran it (its program stepper waits for the world's task queue before it
    returns), and the core runs the game's WaitWorldJobs on the server and the client world before their stop (the
    prediction world stops with the client world, after that wait). No stage is running then; a call found in flight is
    logged as an error. kiln.log names the wait before it starts, says so every so often while it lasts, and gives its
    time when it returns.
  - **A bounded wait only** (the row is unavailable: WaitWorldJobs did not resolve on the build): the 2 s wait is all
    there is. It keeps Kiln's own state alive for a call still running after it, not the game's world: a stage still
    running then can outlive the world's memory.
  - **No stop at all** (the tick hook faulted): no stop event comes; wraps close at their next slice and the game's
    updates run untouched.
- Refused while native SET is loaded.
- Typed views exist for the game build Kiln was made for; on another build a changed program's typed wrap is refused
  (its contract), untyped wraps go on. No typed property for `Dynamic*` components, singletons and other-entity lists;
  no view for the 15 programs with `other_write_unser`, `all_read` or `all_write`.
- Proven on a dedicated server with no player, serial and with the lab parallel switch; a real player's worker split,
  client-world wraps and what a player sees of a wrap's writes have not run in a real game (guided checks
  `06a-wraps.client`, `06a-wraps.waterworks`).
