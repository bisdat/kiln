# Production stations and bases

kiln.stations: the production stations of a base (kilns, smelters, looms, grinders, seed beds: every placed entity with
the game's `FactoryStation`), the bases the game's crafting code sees with their Magic chests, and the game's own station
actions for code: fill from chests, stop, resume, the recipe a station repeats, and moving its finished items into a
chest. C#: `Stations` and `Bases` (`Kiln.Blocks`, `namespace Kiln`). Native: `native/core/abi/services/kiln_stations.h`
(in kiln-items.dll). Since 0.2.0-dev.29.14 (mod API 0.2.6).

**In a mod project** list `Stations` in `KilnServices` (since 0.2.0-dev.29.15): it references Kiln.Blocks' assembly for
`Stations` and `Bases`, and leaves the mod `side: server` with no `clients` need, so a server does not refuse players
without Kiln for a mod that only automates stations. Listing `Blocks` (as with 0.2.0-dev.29.14) also works but makes the mod
`side: both` and `clients: kiln`, which is right only for a mod that defines blocks. With `Stations` alone, a block type or
`Blocks.Define` in the code gets the build warning KILN108 asking for `Blocks`.

`Stations` is the class that already had `Stations.Status` and `Stations.Start` (dev.28, [world-blocks.md](../modding/world-blocks.md)).
`Start` writes the selection without the game's write tracking; for a selection players see at once use `Select`, and
`Read` gives all that `Status` gives and more.

Capabilities (sigdb features, `server.d/stations.json`, the client's hosted world uses the same file):

| Row | What |
|---|---|
| `stations.read` | `Bases.*`, `Stations.Read`, `All`, `InBase`, `Nearest`, `Recipe`, `RecipesOf`, `Categories`, `ResolveInputs`, `Plan`; with `items.move` also `Collect`, `CollectToBase` |
| `stations.fill` | `Stations.Fill` |
| `stations.control` | `Stations.Stop`, `Resume`, `Select` |
| `stations.events` | `StationStateChangedEvent`, `StationPlacedEvent`, `StationRemovedEvent`, `BasesChangedEvent`, `StationCraftedEvent`, `StationPlayerActionEvent` (a subscription is taken before a world runs, in `OnLoad`) |

Where: the server world, on its tick thread: a dedicated server, or a game client while it hosts. A client that joined
another's world has no server world: `Stations.Available` is false there and every call throws `KilnUnavailableException`.

## Bases
A base, for the game's crafting code, is the union of overlapping flame-altar zones. The game rebuilds its list of bases
every server frame (`PlayerBasesSystemServer`, up to 16) with, for each, the containers its crafting may take from:

- **Magic storage chests** (`Prop_Decoration_T1_Storage_24_Magic` and the T2, T3, T4, T6 Magic chests) and the
  player-planted refilling fruit trees and palms: the containers with the game's `InventoryCraftingStock`. **Ordinary
  chests are never in it**: the game's "fill from chests" button and crafting from base storage never use them, neither
  does Kiln, and `Bases.Stock` does not count them (players will expect it to: say so in your mod).
- **Water sources**: wells and water barrels that hold water now.

```csharp
foreach (BaseInfo b in Bases.All)
    Log.Info($"{b} \"{b.Name}\"");                      // id, altars, storage, water, stations; the altar's name (read when asked)
BaseInfo? home = Bases.Of(player);                      // the base the entity stands in (the game's own lookup)
IReadOnlyList<EntityId> chests = Bases.Storage(home!.Id);
IReadOnlyList<EntityId> deposit = Bases.Storage(home.Id, deposit: true); // those that take deposits
IReadOnlyDictionary<ItemId, long> stock = Bases.Stock(home.Id);
long logs = Bases.Stock(home.Id, Items.Lookup("Material_T1_Wood_Logs")!.Id);
EntityId? room = Bases.FindRoomFor(home.Id, charcoalId); // a chest that takes deposits and the item (same item first)
```

**A base can go between two reads.** The game rebuilds its bases every server frame, and an altar placed or removed changes
a base's id: an id read from `Bases.All` a moment ago may name no base by the next call. `Storage`, `WaterSources`,
`Stock`, `Stations.InBase` and `FindRoomFor` then answer empty (or null): no exception to catch in a polling loop (before
0.2.0-dev.29.15 they threw `KeyNotFoundException`). `Bases.Get`, `Of` and `At` answer null as before. Kiln reads a base by
its id and then by the game's record index; when the record was rebuilt in between it finds the base again once.

`Storage(id, deposit: true)` keeps the containers that take deposits, by the two values the game's area deposit
(`area_deposit_trigger`) reads: the entity's `DynamicInventoryCraftingStock` override when it is set
(`{bool canReceiveAreaDepositOverrideIsSet, bool canReceiveAreaDepositOverride}`, from the game's type table), else the
template's `InventoryCraftingStock.canReceiveAreaDeposit`. The lab wrote that override through the game's tracked write
and `Storage(deposit)` followed it. That a player's per-chest switch at the quick-stack station is what writes it (the
game's `inventory_actions` writes the component from a player's input) has not been seen in a game: real-client check C6.
`FindRoomFor` and `CollectToBase` use only those containers.

**Base identity.** `BaseId` is the GUID the game gave a flame altar (its `BaseIdComponent`, kept in the save). A base made
of several overlapping altar zones takes the smallest of its altars' ids, so the id of a base **changes when an
overlapping altar is added or removed**. `BaseInfo.Altars` lists every altar (up to 16) as `BaseAltar(Entity, Id)`: the
entity of this session and the altar's own id, which never changes. Keep per-base data by an altar's id (in `WorldData`):
`EntityData` cannot hold it, because the lab found `EntityData.IsPersistent(altar)` false (it is true for stations and
Magic chests). When the bases are rebuilt, `BasesChangedEvent` says which ids were added, removed, or changed (old and new
share an altar: carry your data over). `BaseInfo.Index` is the game's record index this session and changes with every
rebuild.

## Reading stations
```csharp
IReadOnlyList<EntityId> all = Stations.All();           // every production station of the world
IReadOnlyList<EntityId> mine = Stations.InBase(home.Id);
StationInfo? s = Stations.Read(kiln);                   // null: not a production station (or gone)
Log.Info($"{s!.Template} {s.State} recipe 0x{s.SelectedRecipe:x8}, {s.CraftsLoaded} crafts loaded, {s.Progress:P0}");
foreach (var o in s.Outputs) Log.Info($"finished: {o.Item} x{o.Count}");
EntityId? at = Stations.Nearest(player, workshop: s.Workshop, radius: 8);
```

`StationInfo` holds the game's state (`StationState`: NoRecipe, MissingMaterials, Paused, Stopped, Running,
MissingOutputSpace), the stop flag, what it lacks (`StationNeeds`), the running and the selected recipe (the selected one
repeats), its category choices, the complete crafts it holds now (as the game counts them: the inputs **and the recipe's
water**), the duration and progress of the cycle in progress (paused: where it paused), its water, its template's debug
name, `StopPending` (a stop after the current craft waits), and its inventory. Inputs and outputs share one inventory with
no typed slots: `Outputs` are the stacks of an output of the selected or running recipe that is not also an input of it
(category options count as inputs); `Leftovers` are stacks that are neither (an old recipe's inputs or outputs, which
take room a fill needs).

`Stations.All()` is complete once `Stations.Scanned` is true (the first scan; a world loads its entities for some seconds
after its first step). The server cannot tell which station screen a player has open (`ServerFactoryInfo` was not
traced for that): `Stations.Nearest(player, workshop, radius)` is the stand-in.

Recipes as a station runs them: `Stations.Recipe(id)` (inputs as the data has them, outputs, duration, water, whether it
needs an unlock, `Name`: the game's debug name), `Stations.RecipesOf(station)` (its workshop's recipes),
`Stations.Categories` and `Stations.Category(id)` (the ingredient categories, "any wood", with their options and scaling),
and `Stations.ResolveInputs(recipe, choices)` (the concrete items and counts the game asks for one craft).

**Names for players.** None of these are texts in a player's language. Show the outputs' and inputs' item names
(`Items.Get(id).DisplayName`, in the server's language); categories have ids only. A joined client has no kiln.stations:
a menu there gets ids and names from its server half.

## Fill from chests
`Stations.Fill(station, options)` runs **the game's own "fill from chests"** (the function behind the station screen's
button), with no player: every input is topped up to the crafts asked, taken from the base's Magic chests in the game's
order, the recipe's water poured from the base's water sources; one inventory transaction, all-or-nothing per attempt.
It **never takes from a player's bag** unless `FillOptions.Player` names a player (then the game takes from that
player's inventories first, as its button does; reserves do not apply there).

```csharp
StationResult r = await Stations.Fill(kiln, new FillOptions { Crafts = 10 });            // hold 10 crafts in all
await Stations.Fill(kiln, new FillOptions { Crafts = 2, Mode = FillMode.Add });          // 2 more (the button's meaning)
await Stations.Fill(kiln, new FillOptions { Crafts = 10, Exact = true });                // all 10 or nothing
await Stations.Fill(kiln, new FillOptions { Crafts = 10, DryRun = true });               // what would move
await Stations.Fill(kiln, new FillOptions
{
    Crafts = 50,
    Keep = new Dictionary<ItemId, long> { [logs] = 200 },                                // leave 200 logs in the chests
});
FillPlan p = Stations.Plan(kiln, new FillOptions { Crafts = 10 });                        // no game function runs
Log.Info($"{p.CraftsLoaded} loaded, the stock covers {p.CraftsAffordable} more, a fill would ask for {p.CraftsPlanned}");
if (p.LimitedBy != FillLimit.None) Log.Info($"short: {p.LimitedBy} {p.ShortItem}");      // "waiting for logs (reserve 200)"
```

| Option | Default | What |
|---|---|---|
| `Crafts` | 1 | 1..1000 crafts (see `Mode`) |
| `Mode` | `UpTo` | `UpTo`: the station should hold this many complete crafts in all; `Add`: this many more than it holds |
| `Exact` | false | all of it or nothing (the reserves count: an exact fill never takes into them); otherwise as many crafts as the stock and the station's room cover (`Partial`) |
| `DryRun` | false | the game's function runs and is rolled back: the result says what would move |
| `Water` | true | pour the recipe's water from the base's water sources (off: "loaded" counts the items only) |
| `Recipe`, `Choices` | the station's | fill for another recipe (and its category choices) than the selected one |
| `Player` | none | also take from this player's inventories, first (a player entity only) |
| `Keep` | none | reserves: at least this many of each item stay in the chests (up to 64 items) |

The station must stand in a base (`NoBase`) and have a recipe (`NoRecipe`). When the stock covers fewer crafts than asked,
or the station has no room for them, Kiln asks the game for fewer (halving, at most 6 calls) and says so: `Partial`, with
`CraftsAdded`; `NotEnough` when not even one craft is there, `NoRoom` when the station takes none. `LimitedBy` (`Stock`,
`Reserve`, `Room`, `Water`) and `ShortItem` say what fell short; the plan has the water too (`WaterPerCraft`,
`WaterHave`, `WaterStock`). `Changes` is the ledger: each take from a chest (`From` set) and each put into the station
(`To` set).

A stopped station that gets its inputs starts a cycle for one step and its stop aborts it at once (the inputs go back);
the game does the same after its own fill button. Read the station a few steps after a fill.

"Use all ingredient options" (`StationInfo.UseAllOptions`, `FillPlan.UseAllOptions`): the station may then consume other
options of a category than the one a fill loads, so its loaded crafts can drop faster than a plan says.

## Stop, resume, select
```csharp
await Stations.Stop(kiln);                               // the stop button: now
await Stations.Stop(kiln, StopWhen.AfterCurrentCraft);   // after the running cycle's end
await Stations.Resume(kiln);                             // the stop button again
await Stations.Select(kiln, recipeId);                   // the recipe it repeats (clears a stop)
await Stations.Select(kiln, recipeId, keepStopped: true);
await Stations.Select(kiln, recipeId, allowLocked: true); // even a recipe no one here has unlocked
await Stations.Select(kiln, 0);                          // no recipe (the game aborts the running cycle)
```

- **`StopWhen.Now`** (the default) is what the game's stop button does: **the running cycle is aborted**, the game puts
  its inputs back into the station, and its progress is lost.
- **`StopWhen.AfterCurrentCraft`** waits for the cycle in progress to end (its outputs made, nothing lost), also while it
  is paused (no water under it, no power) or its outputs wait for room, and then stops. Its task completes when the stop
  is written. While it waits, `StationInfo.StopPending` is true. It is **withdrawn**, with `Canceled` and nothing written,
  by a later `Resume`, `Select` or `Stop(Now)` of Kiln's on the station that passed its checks (also in the same step;
  that call's message says it withdrew it) and by a player's or a mod's change of the selected recipe (the message names
  the new recipe); if someone stops the station meanwhile, a player too, it answers `Unchanged`. After
  about an hour of steps it gives up with `TimedOut`, nothing written. A second one on the same station answers
  `Unchanged`. A waiting stop is not saved: a restart forgets it. On a station that runs no cycle it stops at once.
- `Resume` clears the stop: the station starts a new cycle with its selected recipe when its inputs are there.
- `Select` writes the selection as the game writes a player's choice in the station's screen. Choices you pass are
  checked (the category is one of the recipe's inputs, the index one of its options: else `Refused`); without choices the
  station keeps its own. It is refused with `Locked` when no one on the server has unlocked the recipe (the world's shared
  knowledge, or any player in the game), unless `allowLocked`; a recipe of another workshop is `WrongWorkshop`.
- Each returns `Unchanged` when there was nothing to do (already stopped, running, selected).

The stop flag is the game's own (`isStoppedByPlayer`): players see the station stopped and can start it again, and it is
saved with the world (lab: it came back after a restart, with the selection and the inputs). It has no owner: a player, a
mod and Kiln all set the same bit. A mod that stops stations itself keeps its own record next to the station
(`EntityData.Set(station, "stopped-by-me", true)`), clears it when it resumes, and treats a `StationStateChangedEvent`
with `Resumed` and no `ByMod` as "a player started it: my stop no longer stands".

## Collect
```csharp
await Stations.Collect(kiln, chest);                     // every finished stack into this container
await Stations.Collect(kiln, chest, charcoalId);         // only this item
await Stations.Collect(kiln, chest, what: CollectWhat.Leftovers); // what is no input and no output of its recipe
await Stations.CollectToBase(kiln);                      // into the base's chests that take deposits
```
Collect moves through Kiln.Items' move (the game's slot transfer; the row `items.move` too), one stack after the other:
`Done`, `Partial` (some did not fit), `NoRoom` (none did), `Unchanged` (nothing to collect).

## Results
Every action returns a `Task<StationResult>` that completes on the tick thread, one step after it ran (a stop after the
current craft: when written or withdrawn). A refusal is a result, never an exception: `Ok` is false, `Reason` says why,
and nothing changed. `Ok` is true for `Done`, `Partial` and `Unchanged`; `Changed` for `Done` and `Partial`; `CanRetry`
for `QueueFull`, `Dropped`, and a fill's `NotEnough`, `NoRoom` and `Partial` (they change as items move). `StateBefore`
and `StateAfter` are null where nothing was read.

| `StationActionStatus` | When |
|---|---|
| `Done` | it happened |
| `Partial` | fill: fewer crafts than asked (`LimitedBy`); collect: some stacks did not fit |
| `NotEnough` | fill: the stock covers not even one craft (or `Exact` and not all) |
| `NoRoom` | fill: no room in the station; collect: none fit |
| `NotAStation`, `NoBase`, `NoRecipe`, `WrongWorkshop` | as named |
| `Unchanged` | nothing to do |
| `Locked` | select: no one on the server has unlocked the recipe |
| `Refused` | the game refused it, or a category choice does not fit the recipe |
| `Unavailable` | the capability went off after the call |
| `Dropped` | not run: the world closed, or the program did not run it in time |
| `TimedOut` | a stop after the current craft waited about an hour; nothing written |
| `QueueFull` | not taken: 1024 actions are waiting |
| `Canceled` | a stop after the current craft was withdrawn; nothing written |

Exceptions are for mistakes: a station that is `EntityId.None`, `Crafts` outside 1..1000, more than 64 reserves, choices
without a recipe (`ArgumentException`); not on the tick thread (`InvalidOperationException`); the capability off
(`KilnUnavailableException`).

An action is awaited across steps: players and the game act meanwhile. Read the station again after every `await`
rather than reasoning on a `StationInfo` from before it.

## Events
| Event | When |
|---|---|
| `StationStateChangedEvent` | a station's state, stop flag, selected or running recipe changed. `ByMod`: the mod whose stop, resume or select made exactly this change; `Player`: the player whose stop (the flag going on) or select (the selection changing, or the stop clearing) made it, while a `StationPlayerActionEvent` handler listens; both null for the game (also for what the game did after a mod's call, like the state a stop leads to) |
| `StationPlacedEvent`, `StationRemovedEvent` | a production station appeared or is gone (the first scan of a world sets the baseline and posts none) |
| `BasesChangedEvent` | the game rebuilt its bases (an altar placed, removed or upgraded): `Added`, `Removed`, `Changed` (old and new id), `Kept` (a split keeps the old id for one part, a rename does not); the first rebuild after a world starts is a watch's baseline and posts nothing |
| `StationCraftedEvent` | a station finished a cycle: its outputs are in its inventory. Once per cycle |
| `StationPlayerActionEvent` | a player pressed a station button (fill, select, stop, use-all, water) or crafted at a bench (`Action == Craft`: then `Station` is a workbench), before the game runs it: `Cancel()` and the game never runs it |
| `StationChangedEvent` | a Kiln.Stations call changed a station (not a dry run, not `Unchanged`): the record of the call itself, with the result and `ByMod`, the mod that called. For "did my own call do this", use this one |

Every station event carries the `Base` (id) it happened in. They are posted on the tick thread in Pre. Kiln watches
stations only while a handler of `StationStateChangedEvent`, `StationPlacedEvent`, `StationRemovedEvent` or
`BasesChangedEvent` listens (a diff of every station's state each step and a scan for new ones every 32 steps).
`StationCraftedEvent` comes from Kiln.Gameplay's `ItemCraftedEvent`, which since 0.2.0-dev.29.14 reports an automated
station's cycle **once, at its end** (before, twice: at the cycle's start and at its end). `StationPlayerActionEvent` is
Kiln.Players' `PlayerClientActionEvent` of the game's `playerCraftingAction`, typed: while a handler listens, Kiln holds
every player's client actions for one step (about 17 ms) so they can be canceled; canceling either event cancels the same
input (the client is not told: its screen recovers on the next update).

```csharp
Events.On<StationCraftedEvent>(e => Log.Info($"{e.Station} made {string.Join(", ", e.Outputs)}"));
Events.On<StationPlayerActionEvent>(e =>
{
    if (e.Action == StationPlayerAction.Stop && Stations.IsStation(e.Station) && IsLockedStation(e.Station)) e.Cancel();
});
```

**A stocking loop**: react to `StationStateChangedEvent` (MissingMaterials, MissingOutputSpace, a cycle starting) and
`StationCraftedEvent` for stations; nothing fires when a player puts items into a chest, so poll `Bases.Stock` every 2 to
5 seconds for that (it is one native walk over the base's Magic chests; `Bases.Get`, `Storage` and `Stock` do not build
the other bases).

## Who may change what
The actions check nothing about players: they are the server's. A mod that acts for a joined player (its client sends a
message to its server half: `Kiln.Network` mod messages, handled on the server tick, where these calls work) checks
there: the station's base is the player's (`Bases.Of(player)?.Id == Stations.Read(station)?.Base`) and the player may
change it (`Players.Find(player)?.Can(PlayerPermissions.EditBase)`, or `AccessInventories` for fills), or a
`Permissions` node of its own. On a hosting client both halves run in one process and the host's own menu can call
these directly (`Stations.Available` is true there).

## Real-client checks
The lab test (`mods/tests/station-actions-test`) runs on a dedicated server with no player. What only a real game shows is
checked by hand with `mods/tests/stations-client-check` (`/stationcheck`, admins; events in kiln.log as `stationcheck:`):

- **C1** Host a world with the check mod, in a base with a Magic chest and a charcoal kiln: `/stationcheck logs 100`,
  `select`, `fill 5`, `stop`, `resume`, `stop after`, `collect` do what they say on the client's hosted world.
- **C2** With the kiln's screen open: a fill, stop, resume and select from `/stationcheck` show in the screen at once; the
  game's own "fill" button still works and agrees with `plan`.
- **C3** A second player joined: they see the kiln's fire go on and off and its inventory change; on their client
  `stationcheck:` says `Stations.Available = False`.
- **C4** `/stationcheck cancel stop`, then press the kiln's stop button as a player: kiln.log has the
  `StationPlayerActionEvent`, the kiln keeps running, and the screen recovers.
- **C5** After fills, the base's Magic chests are emptied as expected; ordinary chests are untouched.
- **C6** At the quick-stack station, switch one Magic chest off for deposits: `/stationcheck base` lists it under
  Magic chests and not under "take deposits", and `collect` puts nothing into it.

## Limits
- Server world only (dedicated server, hosting client). A joined client has none.
- Magic chests and refilling plants only, as the game: ordinary chests are not the base's stock.
- At most 1024 actions wait at once; 16 a step run.
- `StopWhen.Now` loses the running cycle's progress (the game's rule); `AfterCurrentCraft` keeps it, but is not saved.
- A base's id changes when an overlapping altar is added or removed (see Base identity).
- No "which station screen is open" read (`Stations.Nearest` stands in), and no event when a chest's contents change.
- Kiln adds no hook for this: it calls the game's functions and writes the bytes its own code writes; the only hook
  involved is kiln.gameplay's existing craft hook.

## For native code
`native/core/abi/services/kiln_stations.h` (`kiln.stations` v1, in kiln-items.dll): the same reads (`base_*`, `stations`,
`station_read`, `recipe*`, `category*`, `workshop_recipes`, `plan`) on the tick thread; `submit(owner, request, done, user,
&ticket)` from any thread, the result in `done` on the tick thread; `cancel(ticket)` drops the callback; the events by
polling: `watch(1)`, then `events(after_seq, ...)` each step (`last_seq()` to start at the newest). A cycle's end for native
code is kiln.gameplay's `KILN_GAMEPLAY_CRAFTED` (once per cycle since 0.2.0-dev.29.14). The game's facts behind all of it:
[docs/re/stations.md](../../re/stations.md).
