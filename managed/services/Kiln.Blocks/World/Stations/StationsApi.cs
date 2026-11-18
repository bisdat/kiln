// SPDX-License-Identifier: MIT
// kiln.stations' managed side (in Kiln.Blocks, next to World/StationRuntime.cs' Stations.Status and Start): the production stations of a base and the game's own station actions for code. The native side is
// kiln.stations in kiln-items.dll (native/core/abi/services/kiln_stations.h): bases and their crafting-stock lists read
// from the game's PlayerBasesSystemServer, the game's own "fill from chests" (AutoTransferRecipe) run with no player, and
// the bytes the game's stop and select buttons write. Capabilities stations.read, stations.fill, stations.control,
// stations.events (sigdb features). docs/kiln/services/stations.md.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>
/// The game's bases as its crafting code sees them (since 0.2.0-dev.29.14): each is the union of overlapping flame-altar
/// zones, with the crafting-stock containers the game lists for it every server frame (the Magic storage chests and the
/// player-planted refilling fruit trees: ordinary chests are never in it, as the game's own "fill from chests" and
/// crafting from base storage never use them) and its water sources. Reads on the tick thread of the server world
/// (dedicated server, hosting client); a joined client has none (<see cref="Available"/> false).
/// </summary>
public static class Bases
{
    /// <summary>The capability row of the reads.</summary>
    public const string Capability = Stations.ReadCapability;

    /// <summary>True when kiln.stations reads here (a server world, stations.read on).</summary>
    [AnyThread]
    public static bool Available => StationsCore.Backend.Status() is { Read: true };

    /// <summary>Every base now, in the game's record order.</summary>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">stations.read is off, or no server world here.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static IReadOnlyList<BaseInfo> All
    {
        get
        {
            StationsCore.CheckRead("Bases.All");
            var b = StationsCore.Backend;
            StationsCore.Check(b.BaseCount(out var n), "Bases.All");
            var list = new List<BaseInfo>((int)n);
            for (uint i = 0; i < n; i++)
                if (b.BaseAt(i, out var nb) == StationsNative.Ok) list.Add(StationsCore.InfoOf(nb));
            return list;
        }
    }

    /// <summary>The base with this id; null when none has it now. Builds only that base's record.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static BaseInfo? Get(BaseId id)
    {
        StationsCore.CheckRead("Bases.Get");
        return StationsCore.Find(id, out var nb) ? StationsCore.InfoOf(nb) : null;
    }

    /// <summary>The base with this id, when one has it now.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static bool TryGet(BaseId id, [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out BaseInfo? info)
    {
        info = Get(id);
        return info is not null;
    }

    /// <summary>The base the entity stands in (the game's own lookup by position); null when none.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static BaseInfo? Of(EntityId entity)
    {
        StationsCore.CheckRead("Bases.Of");
        if (entity.IsNone) return null;
        var rc = StationsCore.Backend.BaseOf(entity.Value, out var nb);
        if (rc == StationsNative.NotFound) return null;
        StationsCore.Check(rc, "Bases.Of");
        return StationsCore.InfoOf(nb);
    }

    /// <summary>The base a position lies in (the game's own lookup); null when none.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static BaseInfo? At(WorldPosition position)
    {
        StationsCore.CheckRead("Bases.At");
        var rc = StationsCore.Backend.BaseAtPosition(position, out var nb);
        if (rc == StationsNative.NotFound) return null;
        StationsCore.Check(rc, "Bases.At");
        return StationsCore.InfoOf(nb);
    }

    /// <summary>The base's crafting-stock containers, in the game's order (the order its fill takes from them): the Magic
    /// storage chests and the player-planted refilling fruit trees and palms. With <paramref name="deposit"/>: only those
    /// that take deposits, the containers <see cref="FindRoomFor"/> and <see cref="Stations.CollectToBase"/> use: the
    /// entity's <c>DynamicInventoryCraftingStock</c> override when it is set, else its template's
    /// <c>InventoryCraftingStock.canReceiveAreaDeposit</c> (what the game's area deposit reads).</summary>
    /// <remarks>Empty when no base has this id now: the game rebuilds its bases every frame, and an altar placed or removed
    /// changes a base's id (<see cref="BasesChangedEvent"/>). Before 0.2.0-dev.29.15 that threw <see cref="KeyNotFoundException"/>.</remarks>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static IReadOnlyList<EntityId> Storage(BaseId id, bool deposit = false)
    {
        var all = StationsCore.List(id, water: false, "Bases.Storage");
        return deposit ? all.Where(StationsCore.TakesDeposits).ToArray() : all;
    }

    /// <summary>The base's water sources that hold water (wells, water barrels), in the game's order; empty when no base has
    /// this id now (as <see cref="Storage"/>).</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static IReadOnlyList<EntityId> WaterSources(BaseId id) => StationsCore.List(id, water: true, "Bases.WaterSources");

    /// <summary>What the base's crafting-stock containers hold, per item; empty when no base has this id now (as
    /// <see cref="Storage"/>).</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static IReadOnlyDictionary<ItemId, long> Stock(BaseId id)
    {
        StationsCore.CheckRead("Bases.Stock");
        var rows = StationsCore.ByBase(id, StationsCore.Backend.BaseStock, "Bases.Stock");
        return rows.ToDictionary(r => new ItemId(r.Item), r => (long)r.Count);
    }

    /// <summary><see cref="Stock(BaseId)"/> plus the finished items still in the base's production stations: each station's
    /// <see cref="StationInfo.Outputs"/> (what <see cref="Stations.Collect"/> moves),
    /// which the game's own fill and crafting from base storage never take from. For a target such as "keep 200 charcoal"
    /// that counts what is made but not collected yet. Empty when no base has this id now. Since 0.2.0-dev.29.15.</summary>
    /// <remarks>Reads every station of the base (its inventory): poll it every few seconds, not every step.</remarks>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static IReadOnlyDictionary<ItemId, long> StockWithStations(BaseId id)
    {
        if (StationsCore.Backend.HasStockWithStations)
        {
            StationsCore.CheckRead("Bases.StockWithStations");
            var rows = StationsCore.ByBase(id, i => StationsCore.Backend.BaseStockWithStations(i), "Bases.StockWithStations");
            return rows.ToDictionary(r => new ItemId(r.Item), r => (long)r.Count);
        }
        var stock = new Dictionary<ItemId, long>(Stock(id));
        foreach (var s in Stations.InBase(id))
            if (Stations.Read(s) is { } info)
                foreach (var o in info.Outputs)
                    stock[o.Item] = stock.GetValueOrDefault(o.Item) + o.Count;
        return stock;
    }

    /// <summary>How many of <paramref name="item"/> the base's crafting-stock containers hold.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static long Stock(BaseId id, ItemId item) => Stock(id).TryGetValue(item, out var n) ? n : 0;

    /// <summary>A crafting-stock container of the base that takes deposits (<see cref="Storage"/> with deposit) for
    /// <paramref name="item"/>: one that holds the item already and has room for it, else one with a free slot; null when
    /// none has room.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(Stations.ReadCapability)]
    public static EntityId? FindRoomFor(BaseId id, ItemId item)
    {
        var chests = Storage(id, deposit: true);
        var max = Items.Get(item)?.MaxStack ?? 1;
        EntityId? free = null;
        foreach (var c in chests)
        {
            var inv = Items.Read(c);
            if (inv is null) continue;
            if (inv.Slots.Any(s => s.Item == item && s.Count < max)) return c;
            if (free is null && inv.Slots.Any(s => s.IsEmpty)) free = c;
        }
        return free;
    }
}

// The production stations of a base and the game's own actions on them (the class summary is World/StationRuntime.cs').
public static partial class Stations
{
    /// <summary>Reads: bases, stations, recipes, plans (sigdb feature).</summary>
    public const string ReadCapability = "stations.read";
    /// <summary><see cref="Fill"/> (sigdb feature).</summary>
    public const string FillCapability = "stations.fill";
    /// <summary><see cref="Stop"/>, <see cref="Resume"/>, <see cref="Select"/> (sigdb feature).</summary>
    public const string ControlCapability = "stations.control";
    /// <summary>The station events (sigdb feature).</summary>
    public const string EventsCapability = "stations.events";

    /// <summary>True when kiln.stations reads here (a server world, stations.read on).</summary>
    [AnyThread]
    public static bool Available => StationsCore.Backend.Status() is { Read: true };

    /// <summary>True once Kiln's first scan of the world's stations is done: <see cref="All"/> is complete from then on (a
    /// watch scans every 32 steps; the world loads its entities for some seconds after its first step). Any thread.</summary>
    [AnyThread]
    public static bool Scanned => StationsCore.Backend.Scanned();

    /// <summary>The production station nearest to <paramref name="entity"/> (a player) within <paramref name="radius"/>
    /// metres, optionally of one workshop; null when none. The server cannot tell which station screen a player has open:
    /// this is the stand-in (two stations side by side: the nearer one).</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static EntityId? Nearest(EntityId entity, uint? workshop = null, double radius = 8)
    {
        StationsCore.CheckRead("Stations.Nearest");
        if (World.Position(entity) is not { } at) return null;
        EntityId? best = null;
        var bestD = radius * radius;
        foreach (var s in All())
        {
            if (Read(s) is not { } info || (workshop is { } w && info.Workshop != w)) continue;
            var p = info.Position;
            var d = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
            if (d <= bestD) { bestD = d; best = s; }
        }
        return best;
    }

    /// <summary>Every production station of the world (a scan at most every 32 steps).</summary>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">stations.read is off, or no server world here.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static IReadOnlyList<EntityId> All() => StationsCore.StationList(-1, "Stations.All");

    /// <summary>The production stations standing in a base; empty when no base has this id now (the game rebuilds its bases
    /// every frame, and an altar placed or removed changes a base's id: <see cref="BasesChangedEvent"/>). Before
    /// 0.2.0-dev.29.15 that threw <see cref="KeyNotFoundException"/>.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static IReadOnlyList<EntityId> InBase(BaseId id)
    {
        StationsCore.CheckRead("Stations.InBase");
        return StationsCore.ByBase(id, i => StationsCore.Backend.StationIds((int)i), "Stations.InBase").Select(x => new EntityId(x)).ToArray();
    }

    /// <summary>Whether the entity is a production station now.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static bool IsStation(EntityId entity)
    {
        StationsCore.CheckRead("Stations.IsStation");
        return !entity.IsNone && StationsCore.Backend.StationRead(entity.Value, out _) == StationsNative.Ok;
    }

    /// <summary>A station now, with its inventory and what it would collect; null when the entity is no station.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static StationInfo? Read(EntityId station)
    {
        StationsCore.CheckRead("Stations.Read");
        if (station.IsNone) return null;
        var rc = StationsCore.Backend.StationRead(station.Value, out var s);
        if (rc == StationsNative.NotFound) return null;
        StationsCore.Check(rc, "Stations.Read");
        return StationsCore.InfoOf(s);
    }

    /// <summary>A recipe as a station runs it; null for an unknown id.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static StationRecipe? Recipe(uint id)
    {
        StationsCore.CheckRead("Stations.Recipe");
        return StationsCore.RecipeOf(id);
    }

    /// <summary>The recipes a station can run (its workshop's).</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static IReadOnlyList<uint> RecipesOf(EntityId station)
    {
        StationsCore.CheckRead("Stations.RecipesOf");
        var s = Read(station) ?? throw new ArgumentException($"entity {station} is no production station", nameof(station));
        var (rc, ids) = StationsCore.Backend.WorkshopRecipes(s.Workshop);
        StationsCore.Check(rc, "Stations.RecipesOf");
        return ids.Select(x => x).ToArray();
    }

    /// <summary>Every ingredient category of the game's recipes (22 in its data).</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static IReadOnlyList<IngredientCategory> Categories
    {
        get
        {
            StationsCore.CheckRead("Stations.Categories");
            var list = new List<IngredientCategory>();
            for (uint i = 0; i < 4096; i++)
            {
                if (StationsCore.Backend.CategoryAt(i, out var id) != StationsNative.Ok) break;
                if (StationsCore.CategoryOf(id) is { } c) list.Add(c);
            }
            return list;
        }
    }

    /// <summary>One ingredient category; null for an unknown id.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static IngredientCategory? Category(uint id)
    {
        StationsCore.CheckRead("Stations.Category");
        return StationsCore.CategoryOf(id);
    }

    /// <summary>A recipe's inputs as the game resolves them for a craft: the category choices applied (none: the first
    /// option of each category), counts scaled as the game scales them.</summary>
    /// <exception cref="KeyNotFoundException">The recipe is unknown.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static IReadOnlyList<StationStack> ResolveInputs(uint recipe, IReadOnlyList<CategoryChoice>? choices = null)
    {
        StationsCore.CheckRead("Stations.ResolveInputs");
        var (rc, rows) = StationsCore.Backend.RecipeInputs(recipe, StationsCore.ChoicesOf(choices));
        if (rc == StationsNative.NotFound) throw new KeyNotFoundException($"Stations.ResolveInputs: recipe 0x{recipe:x8} is unknown");
        StationsCore.Check(rc, "Stations.ResolveInputs");
        return rows.Select(r => new StationStack(new ItemId(r.Item), (int)r.Count)).ToArray();
    }

    /// <summary>What <see cref="Fill"/> with these options would do now (no game function runs).</summary>
    /// <exception cref="ArgumentException">The station is none, or an option is out of range.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static FillPlan Plan(EntityId station, FillOptions? options = null)
    {
        StationsCore.CheckRead("Stations.Plan");
        var (r, keep, chests) = StationsCore.FillRequest(station, options ?? new FillOptions());
        return StationsCore.PlanOf(r, keep, chests);
    }

    /// <summary>
    /// Fills a station from its base's crafting-stock chests with the game's own "fill from chests": every input topped up
    /// to the crafts asked, taken from the chests in the game's order, the recipe's water from the base's water sources;
    /// one inventory transaction. Never from a player's bag unless <see cref="FillOptions.Player"/> names one.
    /// </summary>
    /// <returns>Completes on the tick thread one step after it ran. A refusal is a result (<c>Ok</c> false; nothing moved);
    /// <see cref="StationActionStatus.Partial"/> when the stock or the station's room covered fewer crafts than asked.</returns>
    /// <exception cref="ArgumentException">The station is none; <see cref="FillOptions.Crafts"/> is outside 1..1000;
    /// more than 64 reserves; choices without a recipe.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or the calling mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">stations.fill is off, or no server world here.</exception>
    /// <example><code>
    /// var r = await Stations.Fill(kiln, new FillOptions { Crafts = 10, Keep = new Dictionary&lt;ItemId, long&gt; { [logs] = 200 } });
    /// if (!r.Ok) Log.Info($"not filled: {r.Reason}");
    /// </code></example>
    [ServerOnly, TickThreadOnly, RequiresCapability(FillCapability)]
    public static Task<StationResult> Fill(EntityId station, FillOptions? options = null)
    {
        StationsCore.CheckAct(FillCapability, "Stations.Fill");
        var (r, keep, chests) = StationsCore.FillRequest(station, options ?? new FillOptions());
        return StationsCore.Send(r, keep, chests, StationAction.Fill, "Stations.Fill");
    }

    /// <summary>Stops a station as its stop button does. <see cref="StopWhen.Now"/> (the default, the button's meaning)
    /// aborts the running cycle: the game puts its inputs back into the station and its progress is lost.
    /// <see cref="StopWhen.AfterCurrentCraft"/> waits for the running cycle's end first (then nothing is lost); its task
    /// completes when the stop is written.</summary>
    /// <exception cref="ArgumentException">The station is none.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or the calling mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">stations.control is off, or no server world here.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(ControlCapability)]
    public static Task<StationResult> Stop(EntityId station, StopWhen when = StopWhen.Now)
    {
        StationsCore.CheckAct(ControlCapability, "Stations.Stop");
        var r = StationsCore.Request(station, StationsNative.Stop, when == StopWhen.AfterCurrentCraft ? StationsNative.AfterCraft : 0);
        return StationsCore.Send(r, [], [], StationAction.Stop, "Stations.Stop");
    }

    /// <summary>Clears a station's stop (its stop button again): it starts a new cycle with its selected recipe.</summary>
    /// <exception cref="ArgumentException">The station is none.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or the calling mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">stations.control is off, or no server world here.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(ControlCapability)]
    public static Task<StationResult> Resume(EntityId station)
    {
        StationsCore.CheckAct(ControlCapability, "Stations.Resume");
        var r = StationsCore.Request(station, StationsNative.Resume, 0);
        return StationsCore.Send(r, [], [], StationAction.Resume, "Stations.Resume");
    }

    /// <summary>Selects the recipe a station repeats, with its category choices, as a player's choice in the station's
    /// screen does (and clears a stop unless <paramref name="keepStopped"/>). Recipe 0 clears the selection (the game then
    /// aborts the running cycle). A recipe no one on the server has unlocked (the world, or a player in game) is refused
    /// with <see cref="StationActionStatus.Locked"/> unless <paramref name="allowLocked"/>.</summary>
    /// <exception cref="ArgumentException">The station is none, or a choice is out of range.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or the calling mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">stations.control is off, or no server world here.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(ControlCapability)]
    public static Task<StationResult> Select(EntityId station, uint recipe, IReadOnlyList<CategoryChoice>? choices = null,
        bool keepStopped = false, bool allowLocked = false)
    {
        StationsCore.CheckAct(ControlCapability, "Stations.Select");
        var r = StationsCore.Request(station, StationsNative.Select,
            (keepStopped ? StationsNative.KeepStopped : 0) | (allowLocked ? StationsNative.AllowLocked : 0));
        r.RecipeId = recipe;
        StationsCore.SetChoices(ref r, choices);
        return StationsCore.Send(r, [], [], StationAction.Select, "Stations.Select");
    }

    /// <summary>Moves a station's finished items (<see cref="StationInfo.Outputs"/>; <paramref name="what"/> can add or choose
    /// <see cref="StationInfo.Leftovers"/>), or only those of <paramref name="item"/>, into <paramref name="to"/> through
    /// the game's own slot transfer (Kiln.Items' move: needs <c>items.move</c> too).</summary>
    /// <returns><see cref="StationActionStatus.Unchanged"/> when there was nothing to move; <see cref="StationActionStatus.NoRoom"/>
    /// when nothing fit; <see cref="StationActionStatus.Partial"/> when some did.</returns>
    /// <exception cref="ArgumentException">An entity is none.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">stations.read or items.move is off.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static Task<StationResult> Collect(EntityId station, EntityId to, ItemId? item = null, CollectWhat what = CollectWhat.Outputs)
    {
        StationsCore.CheckRead("Stations.Collect");
        if (to.IsNone) throw new ArgumentException("no target", nameof(to));
        return StationsCore.Collect(station, item, what, _ => to);
    }

    /// <summary>As <see cref="Collect"/>, into the station's base: for each stack a crafting-stock chest that takes
    /// deposits and holds the item already, else one with a free slot (<see cref="Bases.FindRoomFor"/>).</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static Task<StationResult> CollectToBase(EntityId station, ItemId? item = null, CollectWhat what = CollectWhat.Outputs)
    {
        StationsCore.CheckRead("Stations.CollectToBase");
        var b = Bases.Of(station);
        return StationsCore.Collect(station, item, what, it => b is null ? null : Bases.FindRoomFor(b.Id, it));
    }
}

/// <summary>What Kiln.Stations asks of kiln.stations (the table, or a test's fake).</summary>
internal interface IStationsBackend
{
    (bool Read, bool Fill, bool Control, bool Events, string Detail)? Status();
    int BaseCount(out uint n);
    int BaseAt(uint index, out StationsNative.Base b);
    int BaseOf(uint entity, out StationsNative.Base b);
    int BaseAtPosition(WorldPosition p, out StationsNative.Base b);
    (int Rc, uint[] Ids) BaseList(uint index, bool water);
    (int Rc, StationsNative.Stack[] Rows) BaseStock(uint index);
    (int Rc, uint[] Ids) StationIds(int baseIndex);
    int StationRead(uint entity, out StationsNative.Station s);
    int Recipe(uint id, out StationsNative.Recipe r);
    (int Rc, StationsNative.Stack[] Rows) RecipeInputs(uint id, StationsNative.Choice[] choices);
    (int Rc, (uint Item, float Scaling)[] Options) Category(uint id);
    int CategoryAt(uint index, out uint id);
    (int Rc, uint[] Ids) WorkshopRecipes(uint workshop);
    int Plan(in StationsNative.Request r, StationsNative.Stack[] keep, uint[] chests, out StationsNative.Plan p);
    int Submit(in StationsNative.Request r, StationsNative.Stack[] keep, uint[] chests, ulong owner, long key);
    int Watch(bool on);
    ulong LastSeq();
    bool Scanned();
    int Events(ulong after, Span<StationsNative.Event> into, out uint count);
    /// <summary>The table has set_output_source / output_sources (kiln.stations of 0.2.0-dev.29.17 or later).</summary>
    bool HasOutputSources { get; }
    /// <summary>The table has max_chests (ordinary chests on a fill).</summary>
    bool HasChests { get; }
    /// <summary>The table has base_stock_with_stations (Magic-chest stock plus finished station outputs).</summary>
    bool HasStockWithStations { get; }
    (int Rc, StationsNative.Stack[] Rows) BaseStockWithStations(uint index);
    int SetOutputSource(uint entity, bool on);
    (int Rc, uint[] Ids) OutputSources();
}

/// <summary>Kiln.Stations' side of kiln.stations: requests, results, reads, the event pump.</summary>
internal static unsafe partial class StationsCore
{

    private sealed class NativeBackend : IStationsBackend
    {
        private static StationsNative.Table* _t;

        private static StationsNative.Table* T
        {
            get
            {
                if (_t != null) return _t;
                var t = Services.Get<StationsNative.Table>(StationsNative.Name, StationsNative.Version);
                // an older kiln.stations (before 0.2.0-dev.29.17) has the table without the output-source members
                if (t == null || t->Header.Size < (uint)StationsNative.TableV1) return null;
                return _t = t;
            }
        }

        public (bool Read, bool Fill, bool Control, bool Events, string Detail)? Status()
        {
            var t = T;
            if (t == null) return null;
            StationsNative.Status s = default;
            s.Size = (uint)sizeof(StationsNative.Status);
            if (t->GetStatus(&s) != StationsNative.Ok) return null;
            return (s.Read != 0, s.Fill != 0, s.Control != 0, s.Events != 0, StationsNative.Text(s.Detail, 160));
        }

        private static StationsNative.Table* Need()
        {
            var t = T;
            if (t == null) throw new KilnUnavailableException(Kiln.Stations.ReadCapability, "kiln.stations is not loaded");
            return t;
        }

        public int BaseCount(out uint n)
        {
            uint x = 0;
            var rc = Need()->BaseCount(&x);
            n = x;
            return rc;
        }

        public int BaseAt(uint index, out StationsNative.Base b)
        {
            StationsNative.Base x = default;
            x.Size = (uint)sizeof(StationsNative.Base);
            var rc = Need()->BaseAt(index, &x);
            b = x;
            return rc;
        }

        public int BaseOf(uint entity, out StationsNative.Base b)
        {
            StationsNative.Base x = default;
            x.Size = (uint)sizeof(StationsNative.Base);
            var rc = Need()->BaseOf(entity, &x);
            b = x;
            return rc;
        }

        public int BaseAtPosition(WorldPosition p, out StationsNative.Base b)
        {
            StationsNative.Base x = default;
            x.Size = (uint)sizeof(StationsNative.Base);
            var v = new StationsNative.Vec3 { X = p.X, Y = p.Y, Z = p.Z };
            var rc = Need()->BaseAtPosition(&v, &x);
            b = x;
            return rc;
        }

        private static (int, uint[]) Ids(delegate* unmanaged[Cdecl]<uint, uint*, uint, uint*, int> f, uint arg)
        {
            var buf = new uint[256];
            for (var round = 0; round < 4; round++)
            {
                uint n = 0;
                int rc;
                fixed (uint* p = buf) rc = f(arg, p, (uint)buf.Length, &n);
                if (rc == StationsNative.BufferTooSmall && n > buf.Length) { buf = new uint[n]; continue; }
                return (rc == StationsNative.BufferTooSmall ? StationsNative.Ok : rc, buf.AsSpan(0, (int)Math.Min(n, (uint)buf.Length)).ToArray());
            }
            return (StationsNative.BufferTooSmall, []);
        }

        public (int Rc, uint[] Ids) BaseList(uint index, bool water) => Ids(water ? Need()->BaseWater : Need()->BaseStorage, index);

        public (int Rc, StationsNative.Stack[] Rows) BaseStock(uint index)
        {
            var buf = new StationsNative.Stack[512];
            for (var round = 0; round < 4; round++)
            {
                uint n = 0;
                int rc;
                fixed (StationsNative.Stack* p = buf) rc = Need()->BaseStock(index, p, (uint)buf.Length, &n);
                if (rc == StationsNative.BufferTooSmall && n > buf.Length) { buf = new StationsNative.Stack[n]; continue; }
                return (rc, buf.AsSpan(0, (int)Math.Min(n, (uint)buf.Length)).ToArray());
            }
            return (StationsNative.BufferTooSmall, []);
        }

        public (int Rc, uint[] Ids) StationIds(int baseIndex)
        {
            var buf = new uint[1024];
            for (var round = 0; round < 4; round++)
            {
                uint n = 0;
                int rc;
                fixed (uint* p = buf) rc = Need()->Stations(baseIndex, p, (uint)buf.Length, &n);
                if (rc == StationsNative.BufferTooSmall && n > buf.Length) { buf = new uint[n]; continue; }
                return (rc, buf.AsSpan(0, (int)Math.Min(n, (uint)buf.Length)).ToArray());
            }
            return (StationsNative.BufferTooSmall, []);
        }

        public int StationRead(uint entity, out StationsNative.Station s)
        {
            StationsNative.Station x = default;
            x.Size = (uint)sizeof(StationsNative.Station);
            var rc = Need()->StationRead(entity, &x);
            s = x;
            return rc;
        }

        public int Recipe(uint id, out StationsNative.Recipe r)
        {
            StationsNative.Recipe x = default;
            x.Size = (uint)sizeof(StationsNative.Recipe);
            var rc = Need()->RecipeInfo(id, &x);
            r = x;
            return rc;
        }

        public (int Rc, StationsNative.Stack[] Rows) RecipeInputs(uint id, StationsNative.Choice[] choices)
        {
            var buf = new StationsNative.Stack[StationsNative.MaxInputs];
            uint n = 0;
            int rc;
            fixed (StationsNative.Choice* c = choices)
            fixed (StationsNative.Stack* p = buf)
                rc = Need()->RecipeInputs(id, c, p, (uint)buf.Length, &n);
            return (rc, buf.AsSpan(0, (int)Math.Min(n, (uint)buf.Length)).ToArray());
        }

        public (int Rc, (uint Item, float Scaling)[] Options) Category(uint id)
        {
            var items = new uint[64];
            var scale = new float[64];
            uint n = 0;
            int rc;
            fixed (uint* i = items)
            fixed (float* s = scale)
                rc = Need()->Category(id, i, s, 64, &n);
            var k = (int)Math.Min(n, 64u);
            var opts = new (uint, float)[k];
            for (var x = 0; x < k; x++) opts[x] = (items[x], scale[x]);
            return (rc, opts);
        }

        public int CategoryAt(uint index, out uint id)
        {
            uint x = 0;
            var rc = Need()->CategoryAt(index, &x);
            id = x;
            return rc;
        }

        public (int Rc, uint[] Ids) WorkshopRecipes(uint workshop) => Ids(Need()->WorkshopRecipes, workshop);

        public int Plan(in StationsNative.Request r, StationsNative.Stack[] keep, uint[] chests, out StationsNative.Plan p)
        {
            StationsNative.Plan x = default;
            x.Size = (uint)sizeof(StationsNative.Plan);
            var copy = r;
            int rc;
            fixed (StationsNative.Stack* k = keep)
            fixed (uint* c = chests)
            {
                copy.Keep = keep.Length > 0 ? k : null;
                copy.ChestCount = (uint)chests.Length;
                copy.Chests = chests.Length > 0 ? c : null;
                rc = Need()->MakePlan(&copy, &x);
            }
            p = x;
            return rc;
        }

        public int Submit(in StationsNative.Request r, StationsNative.Stack[] keep, uint[] chests, ulong owner, long key)
        {
            ulong tk = 0;
            var copy = r;
            fixed (StationsNative.Stack* k = keep)
            fixed (uint* c = chests)
            {
                copy.Keep = keep.Length > 0 ? k : null;
                copy.ChestCount = (uint)chests.Length;
                copy.Chests = chests.Length > 0 ? c : null;
                return Need()->Submit(owner, &copy, &OnDone, (void*)key, &tk);  // the calling mod's owner: a state event names it
            }
        }

        public int Watch(bool on) => T == null ? StationsNative.Unavailable : T->Watch(on ? 1u : 0u);

        public bool HasOutputSources => T is var t && t != null && t->Header.Size >= (uint)StationsNative.TableOutputs;

        public bool HasChests => T is var t && t != null && t->Header.Size >= (uint)StationsNative.TableChests;

        public bool HasStockWithStations => T is var t && t != null && t->Header.Size >= (uint)StationsNative.TableStock &&
                                             t->BaseStockWithStations != null;

        public (int Rc, StationsNative.Stack[] Rows) BaseStockWithStations(uint index)
        {
            if (!HasStockWithStations) return (StationsNative.Unavailable, []);
            var buf = new StationsNative.Stack[512];
            for (var round = 0; round < 4; round++)
            {
                uint n = 0;
                int rc;
                fixed (StationsNative.Stack* p = buf) rc = T->BaseStockWithStations(index, p, (uint)buf.Length, &n);
                if (rc == StationsNative.BufferTooSmall && n > buf.Length) { buf = new StationsNative.Stack[n]; continue; }
                return (rc, buf.AsSpan(0, (int)Math.Min(n, (uint)buf.Length)).ToArray());
            }
            return (StationsNative.BufferTooSmall, []);
        }

        public int SetOutputSource(uint entity, bool on) =>
            HasOutputSources ? T->SetOutputSource(entity, on ? 1u : 0u) : StationsNative.Unavailable;

        public (int Rc, uint[] Ids) OutputSources()
        {
            if (!HasOutputSources) return (StationsNative.Unavailable, []);
            var buf = new uint[64];
            for (var round = 0; round < 4; round++)
            {
                uint n = 0;
                int rc;
                fixed (uint* p = buf) rc = T->OutputSources(p, (uint)buf.Length, &n);
                if (rc == StationsNative.BufferTooSmall && n > buf.Length) { buf = new uint[n]; continue; }
                return (rc, buf.AsSpan(0, (int)Math.Min(n, (uint)buf.Length)).ToArray());
            }
            return (StationsNative.BufferTooSmall, []);
        }

        public ulong LastSeq() => T == null ? 0 : T->LastSeq();

        public bool Scanned()
        {
            var t = T;
            if (t == null) return false;
            StationsNative.Status s = default;
            s.Size = (uint)sizeof(StationsNative.Status);
            return t->GetStatus(&s) == StationsNative.Ok && s.Scanned != 0;
        }

        public int Events(ulong after, Span<StationsNative.Event> into, out uint count)
        {
            uint n = 0;
            int rc;
            fixed (StationsNative.Event* p = into) rc = T == null ? StationsNative.Unavailable : T->Events(after, p, (uint)into.Length, &n);
            count = n;
            return rc;
        }
    }

    internal static IStationsBackend Backend { get; private set; } = new NativeBackend();
    private static readonly object Gate = new();
    private static readonly Dictionary<long, (TaskCompletionSource<StationResult> Tcs, ModContext? Mod, StationAction Action)> InFlight = new();
    private static long _next;

    /// <summary>Test hook: a fake backend (null: the native table again).</summary>
    internal static void UseBackend(IStationsBackend? backend)
    {
        Backend = backend ?? new NativeBackend();
        ResetPump();
    }

    // ---- capability rows: the features of a missing table are lowered (an older kiln-items.dll, the test kit) ----
    internal static readonly ServiceCapRows Rows = new(StationsNative.Name, LowerWithoutService);

    private static void LowerWithoutService()
    {
        try
        {
            if (Backend.Status() is null)
                ServiceCaps.LowerFeatures(StationsNative.Name, Stations.ReadCapability, Stations.FillCapability, Stations.ControlCapability,
                    Stations.EventsCapability, Stations.OutputSourcesCapability);
            else if (!Backend.HasOutputSources)
                ServiceCaps.LowerFeatures(StationsNative.Name, Stations.OutputSourcesCapability);
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException)
        {
            Kiln.Backend.Current.Log(0, LogLevel.Warn, $"kiln.stations: its rows were not lowered: {e.Message}");
        }
    }

    // ---- checks ----
    internal static void CheckRead(string api)
    {
        Rows.Ensure();
        Services.RequireTickThread(api);
        if (!Capabilities.Is(Stations.ReadCapability)) throw ServiceCaps.Unavailable(Stations.ReadCapability, $"{api} cannot read");
        if (Backend.Status() is not { Read: true } s)
            throw new KilnUnavailableException(Stations.ReadCapability, $"{api}: {WhyOff(Backend.Status())}");
    }

    internal static void CheckAct(string capability, string api)
    {
        Rows.Ensure();
        Services.RequireTickThread(api);
        if (!Capabilities.Is(capability)) throw ServiceCaps.Unavailable(capability, $"{api} cannot run");
        var s = Backend.Status();
        var on = capability == Stations.FillCapability ? s is { Fill: true } : s is { Control: true };
        if (!on) throw new KilnUnavailableException(capability, $"{api}: {WhyOff(s)}");
    }

    internal static string WhyOff((bool Read, bool Fill, bool Control, bool Events, string Detail)? s) => s switch
    {
        null => "kiln.stations is not here (an older kiln-items.dll, or no Kiln core)",
        { Detail.Length: > 0 } x => x.Detail,
        _ => "kiln.stations reports it off",
    };

    internal static void Check(int rc, string api)
    {
        switch (rc)
        {
            case StationsNative.Ok: return;
            case StationsNative.WrongThread: throw new InvalidOperationException($"{api}: not on the tick thread");
            case StationsNative.InvalidArgument: throw new ArgumentException($"{api}: kiln.stations refused the arguments");
            case StationsNative.NotFound: throw new KeyNotFoundException($"{api}: not found");
            default: throw new KilnUnavailableException(Stations.ReadCapability, $"{api}: kiln.stations answered {rc} ({WhyOff(Backend.Status())})");
        }
    }

    // ---- reads ----
    internal static BaseInfo InfoOf(in StationsNative.Base b)
    {
        var altars = new List<BaseAltar>();
        var n = Math.Min(b.AltarCount, (uint)StationsNative.MaxAltars);
        fixed (uint* a = b.Altars)
            for (var i = 0; i < n; i++) altars.Add(new BaseAltar(new EntityId(a[i]), new BaseId(StationsNative.AltarIdOf(b, i))));
        return new BaseInfo(new BaseId(StationsNative.GuidOf(b.Id)), (int)b.Index, altars, new WorldPosition(b.Min.X, b.Min.Y, b.Min.Z),
            new WorldPosition(b.Max.X, b.Max.Y, b.Max.Z), (int)b.StorageCount, (int)b.WaterCount, (int)b.ZoneCount, (int)b.StationCount);
    }

    /// <summary>The native record of the base with this id, without building the others.</summary>
    internal static bool Find(BaseId id, out StationsNative.Base found)
    {
        found = default;
        if (id.IsNone) return false;
        var b = Backend;
        if (b.BaseCount(out var n) != StationsNative.Ok) return false;
        for (uint i = 0; i < n; i++)
            if (b.BaseAt(i, out var nb) == StationsNative.Ok && StationsNative.GuidOf(nb.Id) == id.Value)
            {
                found = nb;
                return true;
            }
        return false;
    }

    /// <summary>Whether a crafting-stock container takes deposits: its DynamicInventoryCraftingStock override when set,
    /// else its template's InventoryCraftingStock.canReceiveAreaDeposit. Unknown: yes (a Magic chest's default).</summary>
    internal static bool TakesDeposits(EntityId c)
    {
        try
        {
            if (World.ReadBytes(c, "DynamicInventoryCraftingStock") is { Length: >= 2 } d && d[0] != 0) return d[1] != 0;
            if (Spawn.Inspect(c)?.Template is { } t && Content.Templates.Get(t) is { } entry)
            {
                var v = entry.Get("InventoryCraftingStock.canReceiveAreaDeposit");
                if (v.Type is ContentValueType.Bool or ContentValueType.Int) return v.Bool;
            }
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or NotSupportedException or KeyNotFoundException
                                       or ArgumentException) { }
        return true;
    }

    internal static IReadOnlyList<EntityId> List(BaseId id, bool water, string api)
    {
        CheckRead(api);
        return ByBase(id, i => Backend.BaseList(i, water), api).Select(x => new EntityId(x)).ToArray();
    }

    /// <summary>A read of one base's record by its id: found by id, then read by its record index. The game rebuilds its bases
    /// every frame, so the index can be gone (or another base's) by the read: then it is found again once, and a base no
    /// longer there reads as empty, never as an exception (0.2.0-dev.29.15).</summary>
    internal static T[] ByBase<T>(BaseId id, Func<uint, (int Rc, T[] Rows)> read, string api)
    {
        for (var attempt = 0; attempt < 2; attempt++)
        {
            if (!Find(id, out var b)) return [];
            var (rc, rows) = read(b.Index);
            if (rc == StationsNative.NotFound) continue;  // rebuilt between the two reads
            Check(rc, api);
            // still the same base at that index after the read: else the rows may be another base's
            if (Backend.BaseAt(b.Index, out var after) == StationsNative.Ok && StationsNative.GuidOf(after.Id) == id.Value) return rows;
        }
        return [];
    }

    internal static IReadOnlyList<EntityId> StationList(int baseIndex, string api)
    {
        CheckRead(api);
        var (rc, ids) = Backend.StationIds(baseIndex);
        Check(rc, api);
        return ids.Select(x => new EntityId(x)).ToArray();
    }

    internal static StationRecipe? RecipeOf(uint id)
    {
        if (id == 0) return null;
        var rc = Backend.Recipe(id, out var r);
        if (rc == StationsNative.NotFound) return null;
        Check(rc, "Stations.Recipe");
        var inputs = new List<StationInput>();
        for (var i = 0; i < Math.Min(r.InputCount, (uint)StationsNative.MaxInputs); i++)
            inputs.Add(new StationInput(new ItemId(r.Inputs[i * 4]), (int)r.Inputs[i * 4 + 1], r.Inputs[i * 4 + 2], (int)r.Inputs[i * 4 + 3]));
        var outputs = new List<StationStack>();
        for (var i = 0; i < Math.Min(r.OutputCount, (uint)StationsNative.MaxOutputs); i++)
            outputs.Add(new StationStack(new ItemId(r.Outputs[i * 2]), (int)r.Outputs[i * 2 + 1]));
        string? name = null;
        try { name = Content.Recipes.Get(r.Id)?.Name; }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or NotSupportedException) { }
        string? display = null;
        try
        {
            display = (outputs.Count > 0 ? Items.DisplayName(outputs[0].Item) : null) ?? Stations.RecipeName(r.Id);
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or NotSupportedException) { }
        return new StationRecipe(r.Id, r.Workshop, inputs, outputs, TimeSpan.FromTicks(r.DurationNs / 100), (int)r.WaterCharges,
            r.KnowledgeId == 0) { Name = name, DisplayName = display };
    }

    internal static IngredientCategory? CategoryOf(uint id)
    {
        var (rc, opts) = Backend.Category(id);
        if (rc == StationsNative.NotFound) return null;
        if (rc != StationsNative.Ok && rc != StationsNative.BufferTooSmall) Check(rc, "Stations.Category");
        return new IngredientCategory(id, opts.Select(o => new IngredientOption(new ItemId(o.Item), o.Scaling)).ToArray());
    }

    internal static StationInfo InfoOf(in StationsNative.Station s)
    {
        var station = new EntityId(s.Entity);
        IReadOnlyList<InventorySlot> slots = [];
        try { slots = Items.Read(station)?.Slots ?? []; }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or NotSupportedException) { }
        var shown = s.SelectedRecipe != 0 ? s.SelectedRecipe : s.RunningRecipe;
        var (outputs, leftovers) = SlotRoles(slots, RecipeOf(shown), s.RunningRecipe != shown ? RecipeOf(s.RunningRecipe) : null, CategoryItems);
        var choices = new[] { s.S0, s.S1, s.S2, s.S3 }.Where(c => c.Category != 0).Select(c => new CategoryChoice(c.Category, c.Index)).ToArray();
        var hasWater = s.WaterMax != 0;
        string? template = null;
        try { template = Spawn.Inspect(station)?.TemplateName; }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or NotSupportedException) { }
        return new StationInfo(station, s.Workshop, new WorldPosition(s.Position.X, s.Position.Y, s.Position.Z),
            new BaseId(StationsNative.GuidOf(s.Base)), s.BaseIndex, (StationState)s.State, s.StoppedByPlayer != 0, s.UseAllOptions != 0,
            (StationNeeds)s.Needs, s.RunningRecipe, s.SelectedRecipe, choices, (int)s.CraftsLoaded, TimeSpan.FromTicks(s.DurationNs / 100),
            s.Progress, hasWater ? s.Water : null, hasWater ? s.WaterMax : null, slots, outputs, leftovers,
            (s.Flags & StationsNative.FlagStopPending) != 0, template) { DisplayName = NameOfWorkshop(s.Workshop) };
    }

    private static string? NameOfWorkshop(uint workshop)
    {
        try
        {
            return Stations.WorkshopName(workshop);
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or NotSupportedException)
        {
            return null;
        }
    }

    private static IEnumerable<ItemId> CategoryItems(uint category) =>
        CategoryOf(category)?.Options.Select(o => o.Item) ?? [];

    /// <summary>A station's slots by role for its selected and running recipes: outputs (an output that is no input) and
    /// leftovers (neither an input, a category input's options included, nor an output).</summary>
    internal static (IReadOnlyList<InventorySlot> Outputs, IReadOnlyList<InventorySlot> Leftovers) SlotRoles(IReadOnlyList<InventorySlot> slots,
        StationRecipe? a, StationRecipe? b, Func<uint, IEnumerable<ItemId>> categoryItems)
    {
        var outs = new HashSet<ItemId>();
        var ins = new HashSet<ItemId>();
        foreach (var r in new[] { a, b })
        {
            if (r is null) continue;
            foreach (var o in r.Outputs) outs.Add(o.Item);
            foreach (var i in r.Inputs)
                if (i.IsCategory) ins.UnionWith(categoryItems(i.Category));
                else ins.Add(i.Item);
        }
        var known = a is not null || b is not null;
        outs.ExceptWith(ins);
        var full = slots.Where(x => !x.IsEmpty).ToArray();
        return (full.Where(x => outs.Contains(x.Item)).ToArray(),
            known ? full.Where(x => !outs.Contains(x.Item) && !ins.Contains(x.Item)).ToArray() : []);
    }

    // ---- requests ----
    internal static StationsNative.Request Request(EntityId station, uint action, uint flags)
    {
        if (station.IsNone) throw new ArgumentException("no station", nameof(station));
        return new StationsNative.Request
        {
            Size = (uint)sizeof(StationsNative.Request),
            Action = action,
            Flags = flags,
            Station = station.Value,
        };
    }

    internal static StationsNative.Choice[] ChoicesOf(IReadOnlyList<CategoryChoice>? choices)
    {
        var c = new StationsNative.Choice[4];
        if (choices is null) return c;
        if (choices.Count > 4) throw new ArgumentException("at most 4 category choices (a recipe has at most 4 category inputs)", nameof(choices));
        for (var i = 0; i < choices.Count; i++)
        {
            if (choices[i].Index is < 0 or > 255) throw new ArgumentException($"a choice's index is 0..255 (got {choices[i].Index})", nameof(choices));
            c[i] = new StationsNative.Choice { Category = choices[i].Category, Index = (byte)choices[i].Index };
        }
        return c;
    }

    internal static void SetChoices(ref StationsNative.Request r, IReadOnlyList<CategoryChoice>? choices)
    {
        var c = ChoicesOf(choices);
        r.C0 = c[0];
        r.C1 = c[1];
        r.C2 = c[2];
        r.C3 = c[3];
    }

    internal static (StationsNative.Request Request, StationsNative.Stack[] Keep, uint[] Chests) FillRequest(EntityId station, FillOptions o)
    {
        if (o.Crafts is < 1 or > 1000) throw new ArgumentException($"Crafts is 1..1000 (got {o.Crafts})", nameof(o));
        if (o.Choices is { Count: > 0 } && o.Recipe is null) throw new ArgumentException("category choices name a recipe's categories: give the Recipe too", nameof(o));
        var flags = (o.Mode == FillMode.Add ? StationsNative.FillAdd : 0) | (o.Exact ? StationsNative.FillExact : 0) |
                    (o.DryRun ? StationsNative.DryRun : 0) | (o.Water ? 0 : StationsNative.NoWater) |
                    (o.UseStationOutputs || !Backend.HasOutputSources ? 0 : StationsNative.FillNoOutputs);
        var r = Request(station, StationsNative.Fill, flags);
        r.Crafts = (uint)o.Crafts;
        r.RecipeId = o.Recipe ?? 0;
        r.Player = o.Player.Value;
        SetChoices(ref r, o.Choices);
        var keep = (o.Keep ?? new Dictionary<ItemId, long>())
            .Where(k => k.Value > 0)
            .Select(k => new StationsNative.Stack { Item = k.Key.Value, Count = (uint)Math.Min(k.Value, uint.MaxValue) })
            .ToArray();
        if (keep.Length > StationsNative.MaxKeep) throw new ArgumentException($"at most {StationsNative.MaxKeep} reserves", nameof(o));
        r.KeepCount = (uint)keep.Length;
        var chests = (o.Chests ?? Array.Empty<EntityId>()).Select(e => e.Value).ToArray();
        if (chests.Length > StationsNative.MaxChests)
            throw new ArgumentException($"at most {StationsNative.MaxChests} ordinary chests", nameof(o));
        if (chests.Any(e => e == 0 || e == station.Value || e <= 1023))
            throw new ArgumentException("an ordinary chest is a storage container, not the station and not a player", nameof(o));
        if (chests.Length > 0 && !Backend.HasChests)
            throw new InvalidOperationException("Stations.Fill: this Kiln does not take from ordinary chests");
        r.ChestCount = (uint)chests.Length;
        return (r, keep, chests);
    }

    internal static FillPlan PlanOf(in StationsNative.Request r, StationsNative.Stack[] keep, uint[] chests)
    {
        var rc = Backend.Plan(r, keep, chests, out var p);
        if (rc == StationsNative.InvalidArgument) throw new ArgumentException("Stations.Plan: kiln.stations refused the request's arguments");
        Check(rc, "Stations.Plan");
        var inputs = new List<FillInput>();
        for (var i = 0; i < Math.Min(p.InputCount, (uint)StationsNative.MaxInputs); i++)
            inputs.Add(new FillInput(new ItemId(p.Inputs[i * 2]), (int)p.Inputs[i * 2 + 1], (int)p.Have[i], p.Stock[i])
            {
                Chests = p.Chests[i], Outputs = p.Outputs[i],
            });
        return new FillPlan(StatusOf(p.Status), new EntityId(p.Station), p.RecipeId, (int)p.CraftsLoaded, (int)p.CraftsAffordable,
            (int)p.CraftsPlanned, inputs, StationsNative.Text(p.Message, 128))
        {
            LimitedBy = LimitOf(p.LimitedBy), ShortItem = p.ShortItem != 0 ? new ItemId(p.ShortItem) : null, WaterPerCraft = (int)p.WaterPerCraft,
            WaterHave = (int)p.WaterHave, WaterStock = (int)p.WaterStock, UseAllOptions = p.UseAllOptions != 0,
        };
    }

    internal static FillLimit LimitOf(byte b) => b <= (byte)FillLimit.Water ? (FillLimit)b : FillLimit.None;

    internal static StationActionStatus StatusOf(int s) => Enum.IsDefined(typeof(StationActionStatus), s) ? (StationActionStatus)s : StationActionStatus.Refused;

    internal static Task<StationResult> Send(in StationsNative.Request r, StationsNative.Stack[] keep, uint[] chests, StationAction action, string api)
    {
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var tcs = new TaskCompletionSource<StationResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        long key;
        lock (Gate)
        {
            key = ++_next;
            InFlight[key] = (tcs, mod, action);
        }
        StartPump();
        var owner = Services.CallerOwner;
        if (owner != 0 && mod is not null) lock (Gate) Owners[owner] = mod.Info.Id;
        var rc = Backend.Submit(r, keep, chests, owner, key);
        if (rc == StationsNative.Ok) return tcs.Task;
        lock (Gate) InFlight.Remove(key);
        var station = new EntityId(r.Station);
        if (rc == StationsNative.BufferTooSmall)
            return Task.FromResult(Refusal(StationActionStatus.QueueFull, action, station, r.RecipeId, "1024 station actions are waiting: try again in a step"));
        if (rc == StationsNative.InvalidArgument) throw new ArgumentException($"{api}: kiln.stations refused the request's arguments");
        var cap = action == StationAction.Fill ? Stations.FillCapability : Stations.ControlCapability;
        throw new KilnUnavailableException(cap, WhyOff(Backend.Status()));
    }

    internal static StationResult Refusal(StationActionStatus status, StationAction action, EntityId station, uint recipe, string why) =>
        new(status, action, station, recipe, 0, 0, 0, 0, 0, [], null, null, false, why);

    internal static StationResult ResultOf(in StationsNative.Result r, StationAction action)
    {
        var changes = new List<MoveChange>();
        fixed (byte* c = r.Changes)
        {
            var ch = (StationsNative.MoveChange*)c;
            for (var i = 0; i < Math.Min(r.ChangeCount, (uint)StationsNative.MaxChanges); i++)
                changes.Add(new MoveChange(new SlotId(ch[i].FromSlot), new SlotId(ch[i].ToSlot), new ItemId(ch[i].Item), (int)ch[i].Amount,
                    new EntityId(ch[i].Pide)));
        }
        string message;
        fixed (byte* m = r.Message) message = StationsNative.Text(m, 128);
        // a refusal before the station was read carries no state (DROPPED, LOCKED, UNAVAILABLE, a withdrawn stop)
        var read = r.Status is not (StationsNative.StDropped or StationsNative.StLocked or StationsNative.StUnavailable or StationsNative.StCanceled
            or StationsNative.StTimedOut) || r.StateBefore != 0 || r.StoppedBefore != 0;
        return new StationResult(StatusOf(r.Status), action, new EntityId(r.Station), r.RecipeId, (int)r.CraftsRequested, (int)r.CraftsBefore,
            (int)r.CraftsAfter, (int)r.CraftsAdded, r.GameError, changes, read ? (StationState)r.StateBefore : null,
            read ? (StationState)r.StateAfter : null, action == StationAction.Fill && (r.Flags & StationsNative.DryRun) != 0, message)
        {
            LimitedBy = LimitOf(r.LimitedBy), ShortItem = r.ShortItem != 0 ? new ItemId(r.ShortItem) : null,
            FromChests = (int)Math.Min(r.FromChests, int.MaxValue), FromOutputs = (int)Math.Min(r.FromOutputs, int.MaxValue),
        };
    }

    /// <summary>An action finished (its native callback, or a test).</summary>
    internal static void Complete(long key, in StationsNative.Result r)
    {
        (TaskCompletionSource<StationResult> Tcs, ModContext? Mod, StationAction Action) entry;
        lock (Gate)
        {
            if (!InFlight.Remove(key, out entry)) return;
        }
        var result = ResultOf(r, entry.Action);
        if (result.Changed && !result.DryRun) Changed(result, entry.Mod);
        if (entry.Mod is { Alive: false }) return;  // the asking mod is gone: its continuation is its own code
        entry.Tcs.TrySetResult(result);
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnDone(StationsNative.Result* r, void* user)
    {
        try
        {
            Complete((long)user, *r);
        }
        catch
        {
            // never across into native code
        }
    }
}
