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
| `stations.output_sources` | `Stations.SetOutputSource`, and fills taking from opted-in stations' finished items ([Station outputs as crafting stock](#station-outputs-as-crafting-stock), since 0.2.0-dev.29.17) |
| `stations.craft_sources` | the same opt-in, as the last source of a player's craft ([Station outputs in a craft](#station-outputs-in-a-craft)); lowered at the first world when kiln.rules' payment hook is not in (Creative Mode runs, kiln-rules.dll missing) |
| `stations.craft_ui` | a Kiln client's crafting menu counts those outputs (`client.d/stations-craft-ui.json`); lowered when kiln.rules' crafting-count hook is not in. A vanilla client has neither |
| `stations.stock_ui` | a Kiln client's ingredient rows (the crafting menu's and the station screen's "available") count those outputs ([The station screen's fill button](#the-station-screens-fill-button), `client.d/stations-fill-ui.json`, since 0.2.0-dev.29.19); hooked only once a Kiln server's craft-stock snapshot is on. A vanilla client has none |
| `stations.fill_ui` | a Kiln client's station screen offers its "+1" / "+10" fill when only those outputs can cover it ([The station screen's fill button](#the-station-screens-fill-button), `client.d/stations-fill-ui.json`, since 0.2.0-dev.29.19); its hook goes in only once a Kiln server's craft-stock snapshot is on. A vanilla client has none |

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
var made = Bases.StockWithStations(home.Id);           // + finished items still in the base's stations (0.2.0-dev.29.15)
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

**Names for players** (since 0.2.0-dev.29.15). `StationInfo.DisplayName` is the name of the item that places the station
("Charcoal Kiln"), else its workshop's name; `StationRecipe.DisplayName` is its first output's name ("Charcoal"), else the
recipe's own name text. They come from the game's own data (its workshop and recipe registries, its texts): in the game's
text language on a hosting client; on a dedicated server, whose data has no texts, the English names Kiln has (its shipped
item names, and the catalog's texts when one is built). Categories have ids only; name their options with
`Items.DisplayName`. The first read starts reading the game's names in the background (a fraction of a second): until then
the names are null.

**On a joined client** kiln.stations is not there, but the names are: `Stations.WorkshopName(workshop)`,
`Stations.RecipeName(recipe)` and `Items.DisplayName(item)` read the game's data on any side and any thread, so a menu
there gets ids from its server half and names them in its own player's language. `Ui.ItemImage(item)` draws the items'
icons ([ui.md](../ui.md#item-icons-and-names)). A new recipe of kiln.content is not in the game's registry: `RecipeName` is null for it
(name it by its output's `Items.DisplayName`).

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
| `Keep` | none | reserves: at least this many of each item stay in the Magic chests (up to 64 items). Not applied to a player's bag, to named ordinary chests, or to station outputs |
| `Chests` | none | ordinary chests in this base to take from after the Magic chests and before station outputs (up to 64). Empty: not used |

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

## Station outputs as crafting stock
Since 0.2.0-dev.29.17 (mod API 0.2.9, row `stations.output_sources`; GitHub #68). A station can lend its finished items
to the fills of its base: a charcoal kiln's charcoal fills the forge next to it while it is still in the kiln.

```csharp
Stations.SetOutputSource(charcoalKiln);                  // opt in: true when it is kept with the save
Stations.SetOutputSource(charcoalKiln, on: false);       // opt out
bool lends = Stations.IsOutputSource(charcoalKiln);
IReadOnlyList<EntityId> lenders = Stations.OutputSources(home.Id);  // the base's opted-in stations, in the order used

StationResult r = await Stations.Fill(forge, new FillOptions { Crafts = 5 });
Log.Info($"{r.FromOutputs} item(s) came from station outputs");
await Stations.Fill(forge, new FillOptions { Crafts = 5, UseStationOutputs = false });   // the game's sources only
FillPlan p = Stations.Plan(forge);                       // FillInput.Outputs: what the base's stations lend of each input
```

The rules:
- **Opt-in per station.** Nothing lends by default; a base with no station opted in fills exactly as the game does.
- **Last.** A fill takes from the player's bag (when it names one), then from the Magic chests (after its reserves), then
  from ordinary chests named on that fill, and from the opted-in stations only what those cannot cover, in entity order.
- **Only finished items.** A station lends the stacks `StationInfo.Outputs` lists: an output of its selected or running
  recipe that is not also an input of either. Never its queued inputs (a kiln's logs stay), never its `Leftovers`,
  never an output that is also an input of its recipe.
- **Its own base.** A station lends only to fills in the base it stands in (the game's own base lookup).
- **Checked at the fill.** Inside the fill's transaction, on the server, against the station's inventory and recipe
  then: a station that changed recipe since it was opted in lends what its recipe now makes.
- **All or nothing.** One inventory transaction with the game's own fill: when the stock and the outputs together do not
  cover it, nothing moves (a non-exact fill tries fewer crafts, as before).
- A fill whose recipe names the same item twice (an item and a category option that is that item) does not use
  station outputs.

**The station screen's fill button.** On the server Kiln follows a player's fill presses while any station is opted in
(the players' client actions wait one step for it, about 17 ms, as with `StationPlayerActionEvent`). A press the
player's bag and the Magic chests cannot cover but the base's opted-in outputs can is run as Kiln's fill (the same
recipe, category choices and amount, all or nothing, the player's bag first) and the game's own is canceled; kiln.log
says so. Every other press runs as the game's. Kiln keeps the game's checks: the player must stand in the station's base
and have the right to access inventories. The game's client **disables** its fill buttons ("+1", "+10") when the
player's bag and the Magic chests alone cannot cover the fill (a dry run that does not see the outputs:
[the client code](../../re/stations.md#103-c7-the-station-screens-fill-button)). A **Kiln client** (0.2.0-dev.29.19)
enables them for such a fill when the server says it can run it with station outputs; see
[The station screen's fill button](#the-station-screens-fill-button). On a vanilla client the buttons light only when
the bag and the chests cover the fill, and then the game's own fill runs. Station outputs also serve `Stations.Fill` (a
mod's fill: a command, a Kiln menu) and crafts.

**Kept with the save.** An opt-in is kept beside the save (kiln.saves' entity attachments, the station known by its
template and place, as `EntityData`) with the mod that set it, and comes back after a restart while that mod is loaded
and once kiln.stations' managed side has started (the first use of `Stations`, `Bases` or a station event: a mod that
opts stations in should touch `Stations` in `OnLoad`, for example `Stations.OutputSources()` or an event subscription).
A station removed, or a mod no longer loaded, lends nothing (the record waits for the mod's return). The native set is
per session (`kiln_stations.h`: `set_output_source`, `output_sources`).

## Ordinary chests

A fill can name ordinary storage chests in the station's base (not Magic chests, not refilling plants) and take from them
after the player's bag and the Magic chests, and before opted-in station outputs. Nothing is taken from them unless the
fill names them. The station screen's fill button does not name chests: it stays the player's bag, the Magic chests, and
opted-in station outputs.

```csharp
await Stations.Fill(forge, new FillOptions
{
    Crafts = 5,
    Chests = [woodChest, spareChest],   // this base's ordinary chests, up to 64
});
FillPlan p = Stations.Plan(forge, new FillOptions { Chests = [woodChest] });
// FillInput.Chests: what those chests hold of each input. FillInput.Stock is still the Magic chests and the bag.
```

The rules:
- **Named on the fill.** An empty list is the fill Kiln already did. There is no per-chest opt-in: a chest that should
  always be a source is named on each fill that should use it.
- **This base only.** Each chest is checked again inside the fill's transaction. A chest in another base, a player, an
  NPC (a villager's inventory), the station itself, a production station, a Magic chest, a refilling plant, or an entity
  with no inventory refuses the fill and nothing moves.
- **After the Magic chests, before station outputs.** The player's bag (when the fill names a player) and the Magic
  chests pay first, reserves included on the Magic chests only. Ordinary chests pay the next part, in the order the fill
  names them (a chest named twice counts once, at its first place). Station outputs pay what is still short.
- **All or nothing.** One inventory transaction with the game's own fill. A non-exact fill tries fewer crafts, as before.
- A recipe whose resolved inputs name the same item twice does not use the named chests: they are ignored and the plain fill runs (nothing is taken from them). Station outputs are skipped for the same reason. The fill is not `Refused`.
- Reserves (`Keep`) apply to the Magic chests, not to these chests.

A fill is still the server's action: it does not consult a player's inventory permission. A mod that fills for a joined
player checks that itself, as for every other station action. The game's own button keeps its permission check.

## Player crafting sources

**Since 0.2.0-dev.30.6**, `Crafting.AddSource(player, container, options)` links a storage container to one player for
crafts at stations, at NPCs and in the inventory. Thanks to Fuzen777 for the request (#125).
The container may be in **any base or no base**. It need not be a Magic chest.

```csharp
// Keep the handle for as long as this link should supply the player.
var source = Crafting.AddSource(player, chest, new CraftSourceOptions
{
    Order = CraftSourceOrder.BeforeStationOutputs,
    Filter = CraftSourceFilter.Allow,
    Items = new[] { wood, stone },
});
// When the link is removed:
source.Dispose();
```

The bag and the crafting base's own stock pay first (in the game's order). `BeforeStationOutputs` then pays linked
containers before opted-in station outputs; `AfterStationOutputs` pays them afterwards. Equal-order links use
registration order. Every container counts once **per item**, even if multiple registrations name it or it is already
in the base's own stock. A linked container offers every item the filter allows. Production stations are refused:
`PayMaterial` cannot separate their queued inputs from their finished outputs. Production stations lend their outputs
through `Stations.SetOutputSource`, under its existing per-item safety rules. This does not change `Stations.Fill` or
its fill buttons.

`CraftSourceOptions.Filter` is `Deny` by default: `Items = null` or an empty collection allows every item. `Allow` offers
only the set (an empty set offers nothing). The item ids are copied eagerly on registration: changing the original
collection cannot change an existing source. Dispose and re-register to change it. There is deliberately no `Func`:
native payment can run on a game worker thread, where calling a managed predicate would be unsafe. Mods can evaluate
predicates themselves on the tick thread and register the resulting item set; after item-registry changes they must
rebuild that set if they want new items included.

**Access is the mod's policy plus the game's player permission.** The mod decides which container to link, including a
container in another player's base. Kiln does not infer ownership or enforce that the container shares the crafting
base. At every payment the server checks that the source belongs to **this player**, that the player's live
`BasicPlayerInfos` record grants the game's server-wide `CanAccessInventories` right, that the registration still matches the player login, and that the transaction can read
an initialized storage inventory with no `FactoryStation` component. Registering a non-container, a player's
character/bag, an NPC inventory or a production station is refused. Counts and payment recheck that container type,
so an old registration cannot start supplying a production station after an entity changes or its id is reused.
A client's counts or a message claiming access never authorize payment.

The server still handles only the `RecipePaid` lender path on a verified server-world inventory transaction, never a
client dry run, a storage upgrade or a factory's own cycle. Player mode 0 starts with the bag alone; when
`BaseStockForPlayer` fails (including no base), no base stock or station outputs are added, but independently authorized
linked containers can pay. All cost lines are preflighted, then every take runs through `PayMaterial` **inside the same
recipe transaction**. Any fault or remainder returns the game's failure so `CraftRecipe` rolls everything back. No
two-transaction transfer into the bag is used. The existing duplicate-cost-line and 4096-base-stock-list limits below
still apply.

A **Kiln client** counts the filtered linked stock through `kiln:craftstock` on every `ScreenKind.Crafting` screen:
station, NPC and inventory crafting, including outside a base. The snapshot includes the bag and any permitted current
base stock; it de-duplicates linked containers against that stock and opted-in station outputs. It is only UI help:
the server recounts at payment. A vanilla client keeps the game's menu counts. A missing input snapshot is never
truncated: over 1024 rows the menu keeps the game's counts.

**Lifetime:** `CraftSourceHandle.Dispose()` is idempotent and works on any thread. Unloading the mod removes all of its
sources; an already-running payment may finish. Sources are session-only and cleared when the world closes. They
survive a restart **only if the mod re-registers them**; save the links in the mod's own data, not the disposable handles.
The native registration is also bound to the player login handle: a new player reusing the same entity slot cannot inherit it.
Mods should dispose links when a player leaves and re-register for the new player session.

| Capability | Side and refusal |
| --- | --- |
| `crafting.sources` (`Crafting.SourcesCapability`) | Server or host. Missing payment/access symbols or the permission layout, missing/old `kiln.stations`, or a missing/disabled `kiln.rules` payment hook refuses it. A joined client is the wrong side; offline and no-server-session calls refuse without taking items. |
| `crafting.source_ui` (`Crafting.SourceUiCapability`) | Kiln client. Needs the recipe-input/count symbols and `kiln.rules` crafting-count hook. No client/offline or an unresolved hook means the game's counts remain. |

`Kiln.TestKit` records the real managed API on its fake native table: `h.Crafting.Sources` lists live registrations and
`h.Crafting.Stock(player)` reads registered-only, filtered, permission-checked counts. It models disposal and mod unload,
not the game payment hook, bases, station outputs or screen drawing. Native host tests cover order, filtering and
all-or-nothing preflight; Windows tests exercise the actual guarded `RecipePaid` adapter on fake engine transactions.
The real-game scenarios below are still required.

### Dyes from the same links

**Since 0.2.0-dev.30.6**, the same registrations and copied item filters also supply dyes (`ColorPalette` items).
There is no second registry or dye opt-in. In a Kiln client's tinting screen, linked dyes appear as available even when
none is in the bag. On confirmation the server pays **one dye from the bag first**, otherwise from an accessible linked
container, in registration order. The crafting-specific station-output order does not add station outputs to tinting.
Registered Magic chests count too; dyes do not require the player to stand in their base.

The client receives only filtered **item ids and counts**, never chest slots. It offers source dyes only after this
server explicitly announces dye-source support over `kiln:craftstock`; an older or vanilla server offers none. Closing
the screen, losing the session, or 15 seconds without a refresh removes the offer. Counts are not permission or payment.

The server's Tint action resolves the requested dye afresh, then rechecks this player's registration/login generation,
the item filter, inventory permission and the source's live inventory at payment. Both the bag debit and any source
debit use **the game's own Tint inventory transaction**. A missing dye, revoked access, disposed link or a fault in the
source payment produces the game's failure **before the palette write**, so its rollback restores the inventory.
No temporary dye is transferred to the bag and no separate inventory transaction is committed.

| Capability | Side and refusal |
| --- | --- |
| `crafting.dye_sources` | Server/host Tint entry and payment hooks. Missing tint/transaction/access symbols, changed sparse layouts, a narrowed slot-index wire field or a failed hook refuses source tint requests. |
| `crafting.dye_sources_session` | Needs a running server world and available hooks; no session is pending. A joined client has no server world. |
| `crafting.dye_ui` | Client palette/availability hooks and the full-byte slot-index wire contract. Wrong side, missing symbols/layouts or a failed hook leaves the game's own list. |
| `crafting.dye_ui_session` | Needs this local player's live support/count announcement from a Kiln server. No server, older server, no client world or expired announcement means no source rows. |

The TestKit's `h.Crafting.Dyes(player)` records filtered dye counts from the shared fake registrations, with the same
permission and player-session checks. Native Tint transactions and screen hooks are **offline**, not simulated; their
capability rows say so. Windows tests exercise the production entry/payment callbacks and palette mids on fake engine
transactions; a real client is still the final check.

### Guided check for linked sources

Use a small test mod that can register/dispose one link and change its filter/order. Use a Kiln client and server.

1. Link a chest in a **different base**, put a recipe's missing item there, and leave only part of the cost in the bag
   and current base. At a crafting station check the displayed count, craft once, and verify bag/base pay first and the
   exact shortfall leaves the linked chest. With opted-in station outputs available, check both order values.
2. Walk **outside every base**. In inventory crafting, check that the count includes the linked chest and craft once.
   Verify the bag pays first, then the chest, with no unrelated base inventory touched.
3. Repeat in an **NPC crafting screen**, then in the **inventory screen** in a base. Each displayed count must match the
   items actually removed. Repeat a recipe missing a second ingredient: no output and no partial payment.
4. Allow-list one item and deny-list another. Check both displayed counts and payment; mutate the collection passed at
   registration and confirm the existing source is unchanged.
5. **Dispose** the handle (and separately unload the mod). Within the next stock refresh, extra counts disappear; a
   new craft must not take from that chest. Restart without re-registering and verify the link is gone.
6. Join **two players**. Register for only one: the other must neither count nor pay from the chest. Remove that player's
   `CanAccessInventories` permission and check that a new craft cannot use the link. Restore it and retry. Registering
   either player's bag or a production station as a container must be refused. Opt the station in with
   `Stations.SetOutputSource` instead and confirm only its safe finished outputs lend, never its queued inputs.

### Guided dye check

1. Put a dye **only in a linked chest**, with none in the bag. Open tinting, select the colour and confirm. The palette
   must list it; exactly one dye must leave that chest, the item must change colour, and nothing must be moved into the bag.
2. Put the **same dye in the bag too**. Tint again. Exactly one bag dye must be consumed and the chest must be unchanged.
3. Register a chest for **only another player**. The first player must neither see nor consume its dyes. Also revoke
   inventory permission or dispose a link after opening the palette but before confirming: no dye must leave and no
   colour change may occur. Explicitly linking a permitted ordinary chest in another base remains the mod's choice,
   not an ownership-based refusal. Check an allow/deny filter, reconnect into a reused player slot, and reopen the screen.
4. Join a **vanilla or older server** with a Kiln client. No linked-source dyes should be offered. Close/reopen tinting
   and leave/rejoin a Kiln server: no previous session's source rows should remain.

## Station outputs in a craft
The same opt-in (`Stations.SetOutputSource`) is the last source when a **player** crafts: at a crafting station, by hand,
from any crafting menu. There is no second switch, and the server never reads the client's numbers: it counts again
at the payment.

The game pays a player's craft from the bag, then the base's Magic chests (`rules/InventoryPay`, stock mode 2). It builds
one stock list and uses it for every cost line, so a station cannot be added to that list the way a fill adds one: a
later cost line would take the station's queued inputs. Kiln instead pays the whole recipe itself, inside the game's own
transaction, when all of this holds:
- it is the craft's own payment (the call returns to `rules/RecipePaid`: not a chest upgrade, not the alternate payer);
- it is the server world's transaction (a hosting client's station screen runs a dry run of its own, which stays the
  game's);
- the game's own base and permission test passes: the player stands in a base and has the right to access inventories
  (`BaseStockForPlayer`; a visitor without it, or a payer that is no logged-in player, stays the game's);
- the bag and the chests cannot cover every line, and the opted-in outputs of **that base** cover every line's
  shortfall.

Then it pays line by line: the bag and the chests first, in the game's order, then from the stations that lend that
item, only the rest. A station lends only an output of its selected or running recipe that is not also an input of it,
never its queued inputs, never its Leftovers. When anything fails after a take, the payment answers the game's "not
enough" and the craft rolls back: nothing moves. When the outputs cannot cover every shortfall, Kiln takes nothing and
the game's own payment runs (and refuses the craft). When the bag and the chests cover the craft, the game pays as
always. A recipe that names the same item twice, and a base stock list over 4096 containers, are left to the game.

Only players' crafts use it. A production station's own cycle pays from that station alone (stock mode 0) and never
takes from another. The station being crafted at is never a source: kiln.gameplay's craft hook names it, and
kiln.stations asks for that hook group once a station is opted in (until it is in, a few steps, only a non-player payer
is left out; a player crafts at a crafting station, which lends nothing unless it is also a production station).

Each craft of a "craft x10" is its own payment: the first ones the bag and the chests cover are the game's, the ones
they no longer cover take from the outputs, each all or nothing.

kiln.log (server): `stations: a craft paid N from opted-in station outputs (base B, payer P)`; when Kiln looked and the
outputs were short, `stations: a craft left to the game: base B's opted-in outputs cannot cover it (short by N; payer
P)`; for the first few calls it leaves for another reason, `stations: craft payment left to the game (<why>; ...)`. The
counters of every outcome are kiln-items.dll's `kiln_stations_craft_stats` (labs and support, not an API).

Tested: `stations_test.cpp` (the order, the lenders, all or nothing, a fault or a remainder after a take) and the lab
test `mods/tests/station-craft-test` on a dedicated server (33 PASS: [docs/re/stations.md §10.6](../../re/stations.md#106-lab-dev29-sc2)).
A lab has no logged-in player, so it starts the server with `KILN_STATIONS_CRAFT_LAB=1`: then a spawned player character
(an id outside the player range, so never a real player) gets the base's stock list the game gives a player with the
right. Never set it on a real server; kiln.log warns at start when it is set.

This rides on kiln.rules' hook on the game's inventory payment (installed for this alone when neither free crafting nor
the storage upgrade needs it). With Creative Mode running that hook stands aside, and `stations.craft_sources` is lowered
at the first world with the reason. When Kiln pays a craft, a detour another loader put on `InventoryPay` *before*
Kiln's does not run for that call; one put on it *after* Kiln's makes Kiln stand aside (the return address it sees is
no longer `RecipePaid`), which is safe.

### What each client shows
A crafting menu's "how many" and its craft button follow a count the game's client computes from its own view of the
bag and the base's Magic chests; it does not see a station. Without help it greys a craft only the outputs can complete,
and it does not send a craft its own count does not allow.

A **Kiln client** helps it while a crafting menu is open: it tells the server (on the channel `kiln:craftstock`,
`required: false`) when a menu opens, every 5 s that it is still open, and when it closes. The server then sends that
player, while the player stands in a base where they may access inventories, what the base can pay: per item, the bag,
the Magic chests and the opted-in stations' finished items, only for the items of the recipes that take something those
stations make, again whenever a count changes. The client's count is then the greater of the game's count and the
snapshot's, so a craft only the outputs can complete is offered. When the menu closes the counts are the game's again.
A hosting player's own client gets the same in process. kiln.log on the client: `stations: crafting stock from the
server: N item(s) the crafting menu may count`, `stations: the crafting menu's count for recipe 0x... raised a -> b by
station outputs`, `stations: crafting stock cleared: ...`.

A session ends when the player leaves, when someone else joins into that entity, when the menu closes or stops saying it
is open (15 s), and with a new world; a send that did not go out is sent again. A visitor without the right to access
the base's inventories is sent nothing (as the payment refuses them). A base with any number of item types works: the
snapshot holds only the items those recipes take (up to 1024; beyond that the menu keeps the game's counts and the
server logs it once).

A **vanilla client** has no Kiln, so it never asks and is never sent anything (the channel is not required, and its
connection has none). Its menu shows the game's counts and does not offer a craft only the outputs can complete. A craft
it does send (its count said the bag and the chests cover it) is paid as for anyone: when the server finds the chests
short after all, the outputs still finish it.

### The station screen's fill button
A production station's screen (a forge, a smelter, a charcoal kiln, the alchemist's, the cooking and the other
production stations: every screen of a station with a `FactoryStation`) has a "+1" and a "+10" fill. The game's client
enables each only when its own dry run of that fill goes through on its view of the bag and the Magic chests; a fill
only station outputs can cover stays greyed and is never sent.

A **Kiln client** (since 0.2.0-dev.29.19, row `stations.fill_ui`) helps it while the craft-stock snapshot above is on
(the same session: a Kiln server, a station of the player's base opted in that holds outputs, the right to access the
base's inventories). Then:
- kiln-items hooks the game's UI query for those buttons, on demand: the first time a snapshot is on, never before (no
  hook with a vanilla server or while nothing is lent). kiln.log on the client: `stations: the station screen's fill
  query ... hooked`.
- A fill the game refused for **missing materials** is noted (the station, the recipe, its category choices), and the
  client asks the server about it on `kiln:craftstock` (only the new ones, again after 5 s while unanswered).
- The server answers how many more crafts the button's own press would fill with station outputs now: the same rules as
  the press (the player in the station's base, the right to access inventories, another station of that base opted in;
  the player's bag first, then the Magic chests, then the outputs; the station's room and the recipe's water counted by
  the plan), again whenever that number changes.
- The client's query then says "can fill" for up to that many crafts: "+1" lights when it covers one more craft, "+10"
  when it covers ten. Every other answer stays the game's: a station without room, a missing station inventory, a fill
  the bag and the chests already cover, another recipe or other category choices. kiln.log: `stations: the station
  screen's fill of N craft(s) of recipe 0x... at station S is offered: the server's fill covers it with station
  outputs`.
- The press is the game's own action. The server checks it again when it comes and runs it as Kiln's fill (see
  [above](#station-outputs-as-crafting-stock)); a press that no longer fits (the outputs were taken meanwhile) runs as
  the game's, which refuses it, and nothing moves.

The ingredient rows' **"available"** counts (row `stations.stock_ui`, the same widget on the station screen and in the
crafting menu) follow the same snapshot: for an item the server's snapshot names, a Kiln client shows the greater of the
game's own count (the bag and the Magic chests it sees) and the server's (bag, Magic chests and opted-in outputs). Other
screens that count items (building costs, for example) keep the game's counts: station outputs do not pay those.
kiln.log: `stations: the ingredient rows' available count ... hooked` once, then `stations: an ingredient's available
count for item 0x... shown a -> b with station outputs`. A **vanilla client** keeps the game's buttons and counts.

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
Collect is one `Items.Move` per stack (Kiln.Items' move, the game's slot transfer; it needs the row `items.move`, and
without it the call throws `KilnUnavailableException` for `items.move`), one stack after the other. The target can be
any inventory holder, a player's bag too (`Items.Move` never fills a player's equipped or action-bar slots).
`CollectToBase` asks `Bases.FindRoomFor` for each stack: the first Magic chest that takes deposits and holds the item
with room, else the first with a free slot. The stack goes into that one chest; what it cannot take stays in the station.
`Done` is every stack's move went through (a move that fit only part of its stack counts too), `Partial` some moves went
through and some did not (or found no chest), `NoRoom` none did (also `CollectToBase` of a station that stands in no
base), `Unchanged` nothing to collect. `Changes` lists the rows of every move. A collect that moved items posts a
`StationChangedEvent`.

### Native collect
Native code has `collect` on `kiln.stations` (`KilnStationCollect`, since 0.2.0-dev.29.17). The managed calls above do
not use it. It moves the same stacks (outputs, leftovers, one item) with the same kiln.items move, and its own rules:

- **Target**: `to`, any inventory holder but the station (a player's bag too), or `KILN_STATION_COLLECT_TO_BASE`: the
  base's Magic chests that take deposits, the chests that hold the item with room first, then those with a free slot.
  A stack that one chest takes only in part goes on into the next chest, so one stack can end up in several chests.
- **Status**: `DONE` every stack moved whole. `PARTIAL` some items moved and a stack is not finished. `NO_ROOM` nothing
  moved (also `TO_BASE` and no base). `UNCHANGED` nothing to collect.
- **Ledger**: the result keeps the first 32 ledger rows (`KILN_STATIONS_MAX_CHANGES`) of all the moves.
- **Flags**: the result's `flags` are the collect's own (`KILN_STATION_COLLECT_*`, bits 16 and up). They never read as
  `KILN_STATION_DRY_RUN`.
- **Queue**: it joins the station action queue of `submit` (1024 can wait; a full queue is `KILN_ERR_BUFFER_TOO_SMALL`
  from the call, before anything moves). The callback comes on the tick thread.
- **Watchers and counters**: each move is noted for component watchers as kiln.items' write and counted in kiln.items'
  move and commit counters, as `Items.Move` is.
- **A fault** in a move switches kiln.stations' actions off (as a faulted fill does). Stacks moved before it stay
  moved: the result is `PARTIAL` and says so; with nothing moved yet it is `UNAVAILABLE`.

## Results
Every action returns a `Task<StationResult>` that completes on the tick thread, one step after it ran (a stop after the
current craft: when written or withdrawn). A refusal is a result, never an exception: `Ok` is false, `Reason` says why,
and nothing changed. `Ok` is true for `Done`, `Partial` and `Unchanged`; `Changed` for `Done` and `Partial`; `CanRetry`
for `QueueFull`, `Dropped`, and a fill's `NotEnough`, `NoRoom` and `Partial` (they change as items move). `StateBefore`
and `StateAfter` are null where nothing was read.

| `StationActionStatus` | When |
|---|---|
| `Done` | it happened |
| `Partial` | fill: fewer crafts than asked (`LimitedBy`); collect: some stacks moved and others did not |
| `NotEnough` | fill: the stock covers not even one craft (or `Exact` and not all) |
| `NoRoom` | fill: no room in the station; collect: none fit, or `CollectToBase` and the station stands in no base |
| `NotAStation`, `NoBase`, `NoRecipe`, `WrongWorkshop` | as named (`NoBase` is a fill whose station stands in no base) |
| `Unchanged` | nothing to do |
| `Locked` | select: no one on the server has unlocked the recipe |
| `Refused` | the game refused it, a category choice does not fit the recipe, or a named chest is not an ordinary chest of this base (nothing moves) |
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
The lab tests (`mods/tests/station-actions-test`; ordinary chests and collect: `mods/tests/station-chests-test` together
with the native `mods/tests/station-native-test`) run on a dedicated server with no player. What only a real game shows is
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

Station outputs (GitHub #68). Common setup for C7 to C15, unless a check says otherwise: Kiln 0.2.0-dev.29.17 on every
Kiln machine, the check mod installed where the world runs (the hosting client, or the dedicated server), you an admin.
In a base: a Magic chest, a production station that makes an item a crafting recipe takes (a Smelter with finished
copper bars and copper ore and charcoal still queued, or a Charcoal Kiln with finished charcoal and logs queued), and the
crafting station of that recipe (for copper bars, the Forge). Stand by the production station:
`/stationcheck show` lists its `Outputs` (the finished stacks) and `Leftovers`; `/stationcheck lend` opts it in
(kiln.log: `kiln.stations: station N lends its outputs as crafting stock`). Keep the bag and the Magic chests short of
one craft of the recipe you test by less than the station's finished stack. Startup lines to find in kiln.log first:
server or host `stations: reads on, fill on, ..., craft sources on`, then at the first opt-in `stations: a station is
opted in: kiln.gameplay's craft hook asked for`; a Kiln client `stations: the crafting menu counts opted-in station
outputs when a Kiln server sends them`. A line `stations: station outputs in a craft are off: ...` or `the crafting menu
does not count station outputs here: ...` means a check below cannot pass; note its reason.

- **C7** The station screen's fill buttons (step 1, a Kiln client, 0.2.0-dev.29.19). A forge whose recipe needs the lent
  item (2 charcoal + 2 metal scraps: a charcoal kiln with 20 finished charcoal opted in, the bag and the Magic chests
  holding no charcoal and enough scraps). Open the forge's screen, pick the recipe. Expected: the charcoal row's
  "Available" reads 20 (client kiln.log `the ingredient rows' available count ... hooked` and `... shown 0 -> 20 with station
  outputs`), the scraps row shows the bag and the chests as before, and "+1" and "+10" light up
  (client kiln.log: `the station screen's fill query ... hooked` once, then `the station screen's fill of 1 craft(s)
  ... is offered` and the same for 10). Press "+1": the forge takes 2 charcoal, the kiln's finished stack is 2 lower
  (18), its queued logs unchanged; the server's kiln.log has `kiln.stations: player P's fill of station S ran as Kiln's
  fill with station outputs: ...`. Now "+10" greys within about two seconds (18 charcoal cover 9 crafts), "+1" stays
  lit and the charcoal row's "Available" follows (18). Take the kiln's stack down to 5 by hand: "+1" still lights. Remove the scraps from the bag and the chests: both
  grey (the outputs cannot cover the scraps). Also try the fill hotkey on that screen and note what it does.
- **C16** The same as C7 on a Kiln client joined to a dedicated server with Kiln, and on the hosting client.
- **C17** A vanilla client (no Kiln) on C7's server and base: the buttons stay greyed for the fill only the outputs
  cover and "Available" shows the bag and the chests only; a fill the bag and the chests cover works as always. On a
  Kiln client, the building menu's costs for charcoal still show the bag and the chests only.
- **C18** Other station kinds, each with a recipe that needs an item an opted-in station makes and the bag and the
  chests short of it: a smelter, the alchemist's station, a cooking station (if one of its recipes takes such an item), a
  spinning wheel or loom. Expected: as C7 on each (the "Available" count and the buttons). Also the crafting menu at a
  workbench with a recipe that takes the lent item: its ingredient row's "Available" includes the outputs. A recipe with water: the button lights only when the base's water
  covers it too. Record the station kinds tried.
- **C19** Stale answers. On C7's forge with "+1" lit, take the kiln's charcoal out by hand, then press "+1" at once.
  Expected: nothing moves from the forge's side beyond what the game's own fill would do (the server's kiln.log has the
  player's fill as the game's, or `not even with station outputs` at debug level), and within about two seconds the button
  greys again and "Available" drops. Close the screen: client kiln.log `crafting stock cleared`; reopen it: the buttons and
  the counts follow the new stock.
- **C8** Hosting, Kiln client. Open the crafting station's menu and pick the recipe. Expected: its count and craft button
  allow one craft that the bag and the chests alone could not (kiln.log: `crafting stock from the server: N item(s)` and
  `the crafting menu's count for recipe 0x... raised 0 -> 1 by station outputs`). Craft it. Expected: it completes; the
  lending station's finished stack is lower by exactly the shortfall; its queued inputs are unchanged; the bag and the
  Magic chests are empty of that item (they paid first); kiln.log `stations: a craft paid N from opted-in station outputs
  (base B, payer P)` with N the shortfall. Then a recipe the outputs cannot finish either: its button stays greyed, and
  nothing moves. Close the menu: kiln.log `crafting stock cleared`.
- **C9** A Kiln client joined to a dedicated server with Kiln, the same layout, the client's player in the base with the
  default rights. Expected as C8: the client's kiln.log has the `crafting stock from the server` and `raised` lines, the
  server's the `a craft paid N` line, and the station keeps its queued inputs.
- **C10** A vanilla client (no Kiln) joined to that server. Expected: the count does not include the station and the
  craft button is greyed for the craft only the outputs complete; nothing moves; the server's kiln.log has no `craft paid`
  line for it. A craft the bag and the chests cover still works as always.
- **C11** Rejoin and world switch. On C9's client: leave the server and join again, open the menu: the raised count is
  back (a new `crafting stock from the server` line). On C8's host: load another world with a base set up the same way,
  then this one again: the count follows the world you are in; never a count from the other world.
- **C12** Craft x10. Bag and chests for 8 crafts, the opted-in station's outputs for 2 more. Expected: the menu offers 10;
  "craft 10" makes 10; the server's kiln.log has two `a craft paid` lines (the last two crafts), the station gave exactly
  the two crafts' shortfall and kept its queued inputs. With outputs for only 1 more: the menu offers 9, and a craft of 10
  stops at 9 (the tenth is refused, nothing of it moves).
- **C13** A visitor. A second player (Kiln client) in someone else's base, in a user group without "access
  inventories" (enshrouded_server.json `userGroups`, `canAccessInventories: false`). Expected: their menu shows the game's
  counts (no `crafting stock from the server` line on their client), and a craft they send is not paid from the outputs
  (server kiln.log: `craft payment left to the game (the payer may not access the base's inventories; ...)` the first few
  times). The base owner's own client in the same base still gets the raised counts.
- **C14** A large base: Magic chests holding several hundred different item types (more than 256), and the opted-in
  station. Expected: as C8 (the snapshot names only the items of the recipes that take the lent item), and no
  `crafting stock names more than 1024 items` warning on the server.
- **C15** Nothing opted in (`/stationcheck lend off`). A Kiln client's menu shows the game's counts (a `crafting stock
  cleared` or no `crafting stock from the server` line at all), crafts work as always, and the server's kiln.log has no
  `craft paid` line.

## Limits
- Server world only (dedicated server, hosting client). A joined client has none.
- The game's own stock is Magic chests and refilling plants. Ordinary chests are used when a fill names them, or when a mod registers a player crafting source.
- At most 1024 actions wait at once; 16 a step run.
- `StopWhen.Now` loses the running cycle's progress (the game's rule); `AfterCurrentCraft` keeps it, but is not saved.
- A base's id changes when an overlapping altar is added or removed (see Base identity).
- No "which station screen is open" read (`Stations.Nearest` stands in), and no event when a chest's contents change.
- Hooks: the fills and the station actions call the game's own functions and write the bytes its own code writes.
  Station outputs in a craft ride on kiln.rules' hook on the inventory payment (server or host) and, on a Kiln client,
  its crafting-count hook; kiln.stations also asks for kiln.gameplay's craft hook (the station crafted at) once a station
  is opted in.
- Station outputs pay `Stations.Fill` and players' crafts. The crafting menu counts them on Kiln clients only (a vanilla
  client keeps the game's counts). The station screen's fill buttons and the ingredient rows' "available" counts count
  them on Kiln clients (since 0.2.0-dev.29.19, C7); other screens' item counts do not.

## For native code
`native/core/abi/services/kiln_stations.h` (`kiln.stations` v1, in kiln-items.dll): the same reads (`base_*`, `stations`,
`station_read`, `recipe*`, `category*`, `workshop_recipes`, `plan`) on the tick thread; `submit(owner, request, done, user,
&ticket)` from any thread, the result in `done` on the tick thread; `collect` moves finished stacks through kiln.items' move, with its own rules ([Native collect](#native-collect)). `cancel(ticket)` drops the callback; the events by
polling: `watch(1)`, then `events(after_seq, ...)` each step (`last_seq()` to start at the newest). A FILL names ordinary
chests with `chest_count` and `chests` on a request of the current size (`max_chests()` is `KILN_STATIONS_MAX_CHESTS`; a
request of `KILN_STATION_REQUEST_V1` bytes names none). A recipe whose resolved inputs name one item twice ignores those
chests and runs the plain fill. A named entity that is not an ordinary chest of this base is `KILN_STATION_REFUSED` and
moves nothing. `from_chests` on the result counts what they gave: read it only when `result->size` reaches it (a 29.16
kiln.stations passes a shorter result). Since 0.2.0-dev.29.17 `submit` and `plan` take a request of exactly
`KILN_STATION_REQUEST_V1` or `sizeof(KilnStationRequest)` bytes; 29.16 also took a larger size. `base_stock_with_stations` (check `header.size`) is
`Bases.StockWithStations`: the Magic-chest stock plus finished outputs still in the base's stations, on the tick thread.
`set_output_source(station, on)` and `output_sources(...)` are the opt-in (any thread). For station outputs in a craft
(check `header.size`): `set_craft_stock(rows, n)` is a Kiln client's crafting-menu snapshot (up to
`KILN_STATIONS_MAX_CRAFT_STOCK` rows; any thread; the server's payment never reads it) and `craft_items(items, n, ...)`
the items a client must have counted for those lent items (the inputs of every recipe that takes one; server, tick
thread). kiln-items.dll also exports, for Kiln's own DLLs and labs (not an ABI): `kiln_stations_craft_pay` (kiln.rules'
payment hook), `kiln_stations_craft_adjust` (kiln.rules' crafting-count hook), `kiln_stations_set_craft_context`
(kiln.gameplay's craft hook) and `kiln_stations_craft_stats` (the payment's counters).
A cycle's end
for native code is kiln.gameplay's `KILN_GAMEPLAY_CRAFTED` (once per cycle since 0.2.0-dev.29.14). The game's facts behind
all of it: [docs/re/stations.md](../../re/stations.md).

**Names are not on this table.** `Stations.WorkshopName` and `Stations.RecipeName` stay managed. On a dedicated server
the game's data has no loca texts, and those calls return the English name from Kiln's shipped item catalog and, for a
recipe or workshop tag, from Kiln.Content's English text table. Both live in the managed assemblies. A native reader of
the game's loca table would return null there, which is not what the managed calls return, and a second copy of the
catalog in kiln-items would drift from `Items.DisplayName` (workshop and recipe names ask that first). There is no
native slot for them.

## Changelog

- **0.2.0-dev.30.6:** player crafting sources (`Crafting.AddSource`), filtered linked containers in any base or none,
  ordered around opted-in station outputs, disposable/mod-owned lifetime, atomic recipe payment and Kiln-client crafting
  counts, plus the tinting screen and server-authoritative dye payment from the same player sources. Thanks Fuzen777 (#125).
