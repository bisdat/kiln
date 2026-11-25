// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessClientStations? _clientStations;

    /// <summary>The fake kiln.stations.client of this harness: the stations the game client's own station cache holds, as
    /// <see cref="ClientStations"/> reads them. Since 0.2.0-dev.31.13.</summary>
    public HarnessClientStations ClientStations => _clientStations ??= new HarnessClientStations(this);
}

/// <summary>
/// The client's station cache of a harness (<see cref="KilnHarness.ClientStations"/>): what <see cref="Kiln.ClientStations"/>
/// reads on a joined client. Since 0.2.0-dev.31.13.
/// </summary>
/// <remarks>
/// A test adds stations as the server would have sent them (<see cref="Add"/>), the recipes the client's data has
/// (<see cref="SetRecipe"/>) and moves the client's clock (<see cref="Clock"/>). Every read computes the view as
/// kiln-items.dll does: one craft is the recipe's duration times <see cref="SpeedFactor"/> (float, as the game); while a
/// station is Running, what is left of the cycle and of all its loaded crafts (the game HUD's arithmetic) and the progress;
/// finished outputs from the station's <see cref="FakeClientStation.Inventory"/> (null: the client has no copy of it).
/// Positions read in the cache's 0.25 m steps. The row <c>stations.client_view</c> can be switched off
/// (<c>kiln.Capabilities.Override</c>).
/// </remarks>
public sealed class HarnessClientStations
{
    private readonly KilnHarness _kiln;

    internal HarnessClientStations(KilnHarness kiln) => _kiln = kiln;

    private StationsClientState S => KitFakes.State<StationsClientState>(_kiln);

    /// <summary>The client's game clock (what the station's cycle start is measured on). Starts at zero.</summary>
    public TimeSpan Clock
    {
        get
        {
            lock (S.Gate) return TimeSpan.FromTicks(S.ClockNs / 100);
        }
        set
        {
            lock (S.Gate) S.ClockNs = value.Ticks * 100;
        }
    }

    /// <summary>The client's production speed factor (the game's FactorySpeed: a craft's time is multiplied by it). 1 by
    /// default; 0 or less: no time is known.</summary>
    public float SpeedFactor
    {
        get
        {
            lock (S.Gate) return S.Factor;
        }
        set
        {
            lock (S.Gate) S.Factor = value;
        }
    }

    /// <summary>How many reads mods made of the client's station view.</summary>
    public int Reads
    {
        get
        {
            lock (S.Gate) return S.Reads;
        }
    }

    /// <summary>Puts a station into the client's cache (or returns the one there): no recipe, state NoRecipe. Change it through
    /// the returned object; every read sees its current values.</summary>
    public FakeClientStation Add(EntityId station, uint workshop, WorldPosition position, BaseId baseId = default)
    {
        if (station == EntityId.None) throw new ArgumentException("a station entity", nameof(station));
        lock (S.Gate)
        {
            if (S.Stations.FirstOrDefault(s => s.Station == station) is { } there) return there;
            var s = new FakeClientStation(station) { Workshop = workshop, Position = position, Base = baseId };
            S.Stations.Add(s);
            return s;
        }
    }

    /// <summary>The station in the client's cache, or null.</summary>
    public FakeClientStation? Get(EntityId station)
    {
        lock (S.Gate) return S.Stations.FirstOrDefault(s => s.Station == station);
    }

    /// <summary>Takes a station out of the client's cache (the server stopped sending it, its base went).</summary>
    public bool Remove(EntityId station)
    {
        lock (S.Gate) return S.Stations.RemoveAll(s => s.Station == station) > 0;
    }

    /// <summary>Empties the client's cache (a world change).</summary>
    public void Clear()
    {
        lock (S.Gate) S.Stations.Clear();
    }

    /// <summary>A recipe as the client's data has it: its unscaled duration, its outputs and its input items (a category
    /// input's options count as inputs). A recipe the test never sets is one the client does not know: no time, no
    /// outputs.</summary>
    public void SetRecipe(uint recipe, TimeSpan duration, IReadOnlyList<ItemId> outputs, IReadOnlyList<ItemId>? inputs = null)
    {
        if (recipe == 0) throw new ArgumentException("a recipe id", nameof(recipe));
        ArgumentNullException.ThrowIfNull(outputs);
        lock (S.Gate)
            S.Recipes[recipe] = (duration.Ticks * 100, outputs.Select(o => o.Value).ToArray(), (inputs ?? []).Select(i => i.Value).ToArray());
    }
}

/// <summary>One station of the client's cache in a harness (<see cref="HarnessClientStations.Add"/>). Since
/// 0.2.0-dev.31.13.</summary>
public sealed class FakeClientStation
{
    internal FakeClientStation(EntityId station) => Station = station;

    /// <summary>The station.</summary>
    public EntityId Station { get; }

    /// <summary>Its workshop.</summary>
    public uint Workshop { get; set; }

    /// <summary>Where it stands (read in 0.25 m steps).</summary>
    public WorldPosition Position { get; set; }

    /// <summary>The base the client grouped it under.</summary>
    public BaseId Base { get; set; }

    /// <summary>The selected recipe; 0: none.</summary>
    public uint Recipe { get; set; }

    /// <summary>Its state as the server sent it.</summary>
    public StationState State { get; set; }

    /// <summary>What it lacks.</summary>
    public StationNeeds Needs { get; set; }

    /// <summary>The complete crafts its inputs still hold after the cycle in progress.</summary>
    public int CraftsLoaded { get; set; }

    /// <summary>The cycle's start on the client's clock (<see cref="HarnessClientStations.Clock"/>).</summary>
    public TimeSpan Started { get; set; }

    /// <summary>Its water in Fuel units (64 per charge).</summary>
    public int Water { get; set; }

    /// <summary>Its water capacity in Fuel units.</summary>
    public int WaterMax { get; set; }

    /// <summary>"Use all ingredient options".</summary>
    public bool UseAllOptions { get; set; }

    /// <summary>It runs on rotational energy.</summary>
    public bool NeedsEnergy { get; set; }

    /// <summary>The selected recipe's category choices (at most 4 are read).</summary>
    public List<CategoryChoice> Choices { get; } = [];

    /// <summary>The client world's copy of the station's inventory; null: the client has none (outputs unknown).</summary>
    public List<StationStack>? Inventory { get; set; }

    /// <summary>Runs <paramref name="recipe"/>: state Running, its cycle started at <paramref name="started"/>, with
    /// <paramref name="craftsLoaded"/> more crafts loaded.</summary>
    public FakeClientStation Run(uint recipe, int craftsLoaded, TimeSpan started)
    {
        Recipe = recipe;
        CraftsLoaded = craftsLoaded;
        Started = started;
        State = StationState.Running;
        return this;
    }
}
