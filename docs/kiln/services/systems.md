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
  (`Disabled`) instead of skipping every slice; an empty slice is skipped.
- **Lifetime.** `Wrap` is tick thread and returns at once; the stages apply in the target world's next step
  (`State` Pending → Active), again by program name in every later world of that kind, and go with `Dispose` (applied in
  the next step: no call starts after it) or when the mod unloads (the unload waits until no call of the mod's stages
  runs, up to 2 s; a still-running callback remains rooted and is reaped after it finishes). `Stats` sums calls, entities, time, faults and budget overruns over the wrap's stages.
- **Refusals.** `Ok = false`: the program is not in that world, it has no update there, native SET is loaded (it
  replaces the game's program runner, so Kiln stays out of it and takes existing wraps out). A capability that is off
  throws `KilnUnavailableException`; the prediction world has no table of its own (`ArgumentException`).

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
  contract is its own (asked once per world session), so a view never reads a slot of another layout.
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
  under a `[Query]`'s key with other lists, more than 8 programs, a bad name or a query without an entity list stops the
  build (KILN036). A mod built without the Kiln SDK writes the file itself in the same form
  (`schema/kiln-programs.schema.json`).
- **Results.** `ProgramRegistration`: `Ok = false` with `"not reserved at boot: rebuild the mod and restart the server"`
  when kiln.world reserved nothing under the key for the mod (the file is missing, or the mod was loaded after the server
  started), or with kiln.world's refusal (another binding holds it). `ArgumentException`: the struct has no `[Query]`,
  the key or the options are not the `[Query]`'s, or a name the iterator gives a slot (templates, reads, writes,
  singletons, other-entity and event lists) is not in the running reservation: a mod built from other sources than the
  one the server reserved. The filters (`With`, `Without`, `AnyOf`) and the flags are checked against the `[Query]`
  only. `KilnUnavailableException("systems.define")` when kiln.world's programs did not resolve on the build.
- **The handler** gets the query for one entity; the query reads and writes through `EcsProgramContext` (`TryGet`,
  `TrySet`, `TryReadOther`, events, `QueueAdd`), as `EcsPrograms.Bind` does. Worker threads: no tick-thread API inside
  it; a handler that throws is turned off with a line in the mod's log. `Dispose` (tick thread) unbinds it and waits
  until no call runs; the mod's unload does too. The reservation stays, inert, until the server restarts, and a reload
  of the mod binds it again.
- `[ServerOnly]` (the server world, also on a host). Lab: `mods/tests/systems-test` binds one in `OnLoad`, checks it ran
  for `fuel_setup`'s containers, that a query without a reservation is `Ok = false`, and its handler on N > 1 threads with
  the lab parallel switch.

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
- Refused while native SET is loaded.
- Typed views exist for the game build Kiln was made for; on another build a changed program's typed wrap is refused
  (its contract), untyped wraps go on. No typed property for `Dynamic*` components, singletons and other-entity lists;
  no view for the 15 programs with `other_write_unser`, `all_read` or `all_write`.
- Proven on a dedicated server with no player, serial and with the lab parallel switch; a real player's worker split,
  client-world wraps and what a player sees of a wrap's writes have not run in a real game (guided checks
  `06a-wraps.client`, `06a-wraps.waterworks`).
