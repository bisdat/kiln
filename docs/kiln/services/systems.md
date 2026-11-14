# Kiln.Systems: wraps on the game's ECS programs

Your code before, after or instead of one of the game's own ECS programs (`fuel_setup`, `health_recharge`, ...), per
slice or per entity, in the program's own slot of the scheduler and on its thread. Managed: `Kiln.Systems`
(`KilnServices` `Systems`); native: kiln.world v4 (`native/core/abi/services/kiln_world.h`, "game programs"). Server
world. Since 0.2.0-dev.29.

| Capability | What it says |
|---|---|
| `systems.wraps` | wraps work on this build; unavailable while native SET is loaded (reason `policy`) |
| `systems.wraps.client` | wraps on the client world's programs; held until the guided client check `06a-wraps.client` ([caps.md](caps.md#the-client-gate-dev28)) |
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
  `WhenArchetype(has, lacks)` decides once per archetype and world (component names as in kiln.world).
- **Lifetime.** `Wrap` is tick thread and returns at once; the stages apply in the target world's next step
  (`State` Pending → Active), again by program name in every later world of that kind, and go with `Dispose` (applied in
  the next step: no call starts after it) or when the mod unloads (the unload waits until no call of the mod's stages
  runs, at most 2 s). `Stats` sums calls, entities, time, faults and budget overruns over the wrap's stages.
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

### Change events

A stage's writes through a view are compared when the stage is done with the entity. Changed components are coalesced
per step per entity and component (the bytes before the first stage that wrote, after the last one, every writer
once), and at the next step's start Kiln raises `ComponentWrittenEvent` on the tick thread, once each, with
`Writer = "<modid>/wrap:<program>"` (`"my-mod/wrap:fuel_setup"`) and `Writers` listing every wrap that wrote it in that
step. `Result.Before` and `Result.After` span everything between those two stages, the game's own update included when
a Before and an After both wrote. Nothing is copied or compared while nobody listens for the event. A store through a
pointer kept past the call is neither marked nor raised. Server world only.

## In the lab: the parallel switch

On a dedicated server with no player every slice runs on the world's thread. `Kiln.Testing.LabSystems`
(`KILNX001`, needs `kiln/control/systems.parallel`) forces the game's own worker split for up to 3000 steps, so a test
can run its handlers on N > 1 threads: `LabSystems.ForceParallel(steps)`, `StopParallel()`, `Forced`, `PlayersMask`.
`mods/tests/systems-test` runs every row serial and with the switch on.

## Native: kiln.world v4

The C table appends the game-program members to kiln.world (`KILN_WORLD_VERSION 4`): `game_program_find`, `_info`,
`_list`, `game_programs_status`, `game_program_wrap` / `_unwrap`, `game_wrap_info`, `game_wraps_release`,
`game_slice_run_original`, `wrap_original`, and per entity inside a stage `game_slice_iterate`, `game_slice_slot`,
`game_slice_mark`, `game_slice_attr_changed`, `game_slice_archetype`, `game_archetype_has`. The header documents each
(threads, results); `mods/tests/systems-test/native` uses all of them. A native stage's fault (SEH) is caught and turns
that stage off; `KilnWrapStageFn` runs on the slice's thread.

## Limits

- One Replace per program; no per-entity Replace.
- The server world. Client-world wraps (`WrapOptions.World = WorldKind.Client`) compile and stay refused until
  `systems.wraps.client` opens; the prediction world has no table.
- Refused while native SET is loaded.
- Per-entity views name components by string; generated typed views per program are the next step (planned).
- Proven on a dedicated server with no player, serial and with the lab parallel switch; a real player's worker split and
  what a player sees of a wrap's writes have not run in a real game (guided check `06a-wraps.waterworks`).
