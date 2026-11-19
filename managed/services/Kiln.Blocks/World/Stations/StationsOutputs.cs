// SPDX-License-Identifier: MIT
// Station outputs as crafting stock (GitHub #68, 0.2.0-dev.29.17): a station opted in lends its finished items to every
// fill in its base, last. The native side (kiln.stations' set_output_source, and the fill that asks the game's own
// AutoTransferRecipe for each input's shortfall with a source list of only the lending stations) is in stations.cpp;
// this is the opt-in (kept beside the save in kiln.saves' entity attachments, scope "kiln:stations", while the mod that
// set it is loaded) and the station screen's own button (a fill the player and the Magic chests cannot cover but the
// outputs can runs as Kiln's fill, the game's canceled). docs/kiln/services/stations.md "Station outputs as crafting
// stock", evidence docs/re/stations.md section 8.
using System.Buffers.Binary;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

public static partial class Stations
{
    /// <summary><see cref="SetOutputSource"/> and the fills that take from opted-in stations (sigdb feature).</summary>
    public const string OutputSourcesCapability = "stations.output_sources";

    /// <summary>The same opt-in, as the last source of a player's craft (sigdb feature; it also needs kiln.rules' payment
    /// hook, and is lowered at the first world without it). A station's own craft never takes from other stations.</summary>
    public const string CraftSourcesCapability = "stations.craft_sources";

    /// <summary>A Kiln client's crafting menu counts the opted-in outputs the server sends (sigdb feature, client; it also
    /// needs kiln.rules' crafting-count hook). A vanilla client keeps the game's counts.</summary>
    public const string CraftUiCapability = "stations.craft_ui";

    /// <summary>
    /// Opts a station in (or out) as crafting stock: its finished items (<see cref="StationInfo.Outputs"/>) become the last
    /// source of every fill in its base, the game's "fill from chests" button included: after the player's bag and the
    /// Magic chests, only what they cannot cover. Never its queued inputs, never its <see cref="StationInfo.Leftovers"/>,
    /// never an output that is also an input of its recipe; never for a fill in another base. Checked again at each fill
    /// against the station's inventory and recipe then. Opted out (the default) a station is never touched. Since
    /// 0.2.0-dev.29.17.
    /// </summary>
    /// <returns>True when the setting is kept with the save (it comes back after a restart while the calling mod is
    /// loaded); false when it holds for this session only (an entity the save does not keep, or kiln.saves is off).
    /// Opting out returns true when nothing is left kept.</returns>
    /// <exception cref="ArgumentException">The entity is none, or no production station.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread; or 4096 stations are opted in already.</exception>
    /// <exception cref="KilnUnavailableException"><c>stations.output_sources</c> is off, or no server world here.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(OutputSourcesCapability)]
    public static bool SetOutputSource(EntityId station, bool on = true)
    {
        StationsCore.CheckOutputs("Stations.SetOutputSource");
        if (station.IsNone) throw new ArgumentException("no station", nameof(station));
        if (on && !IsStation(station)) throw new ArgumentException($"entity {station} is no production station", nameof(station));
        var rc = StationsCore.Backend.SetOutputSource(station.Value, on);
        if (rc == StationsNative.BufferTooSmall) throw new InvalidOperationException("Stations.SetOutputSource: 4096 stations are opted in already");
        if (rc != StationsNative.Ok)
            throw new KilnUnavailableException(OutputSourcesCapability, $"Stations.SetOutputSource: {StationsCore.WhyOff(StationsCore.Backend.Status())}");
        StationsCore.StartPump();
        return StationOutputKeeper.Record(station, on, ModContext.CurrentMod?.Info.Id);
    }

    /// <summary>Whether the station is opted in as crafting stock now (<see cref="SetOutputSource"/>). False where
    /// <c>stations.output_sources</c> is off.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static bool IsOutputSource(EntityId station)
    {
        StationsCore.CheckRead("Stations.IsOutputSource");
        return !station.IsNone && StationsCore.SourceIds().Contains(station.Value);
    }

    /// <summary>The stations opted in as crafting stock now, in the order a fill takes from them (entity order); with
    /// <paramref name="base"/> only those standing in that base. Empty where <c>stations.output_sources</c> is off.</summary>
    [ServerOnly, TickThreadOnly, RequiresCapability(ReadCapability)]
    public static IReadOnlyList<EntityId> OutputSources(BaseId? @base = null)
    {
        StationsCore.CheckRead("Stations.OutputSources");
        var ids = StationsCore.SourceIds().Select(x => new EntityId(x));
        if (@base is { } b) ids = ids.Where(e => Read(e)?.Base == b);
        return ids.ToArray();
    }
}

internal static partial class StationsCore
{
    private static EventSubscription? _sourceFills;

    internal static void CheckOutputs(string api)
    {
        Rows.Ensure();
        Services.RequireTickThread(api);
        if (!Capabilities.Is(Stations.OutputSourcesCapability)) throw ServiceCaps.Unavailable(Stations.OutputSourcesCapability, $"{api} cannot run");
        if (Backend.Status() is not { Read: true, Fill: true } s || !Backend.HasOutputSources)
            throw new KilnUnavailableException(Stations.OutputSourcesCapability, $"{api}: {WhyOff(Backend.Status())}");
    }

    /// <summary>The opted-in stations native holds now (empty without the table's members).</summary>
    internal static HashSet<uint> SourceIds()
    {
        if (!Backend.HasOutputSources) return [];
        var (rc, ids) = Backend.OutputSources();
        return rc == StationsNative.Ok ? [.. ids] : [];
    }

    /// <summary>Every service tick (the stations pump): the kept opt-ins' consumer, their pruning, and the button's follow.</summary>
    internal static void PumpOutputs(ulong step)
    {
        if (!Backend.HasOutputSources || Backend.Status() is not { Read: true }) return;
        StationOutputKeeper.Register();
        if (step % 25 == 0) StationOutputKeeper.Prune();
        var want = StationOutputKeeper.Count > 0 && Capabilities.Is(Stations.OutputSourcesCapability) && Capabilities.Is(Stations.FillCapability);
        Follow(ref _sourceFills, want, () => Events.On<PlayerClientActionEvent>(OnSourceFill, EventPriority.Low));
    }

    private static void ResetOutputs()
    {
        _sourceFills?.Dispose();
        _sourceFills = null;
    }

    /// <summary>Tests: whether the button's follow is on.</summary>
    internal static bool FollowsSourceFills => _sourceFills is not null;

    /// <summary>Whether the player may take from the base's inventories (the game's own check before its fill: the
    /// permission bit CanAccessInventories); null when it is not known (then the game decides, with its own fill).</summary>
    internal static Func<Player, bool?> CanAccessInventories = static p =>
        p.PermissionsKnown ? p.Can(PlayerPermissions.AccessInventories) : null;

    // The station screen's "fill from chests" (playerCraftingAction type 1), held one step by Kiln.Players. Runs after
    // Stations' own typed event (EventPriority.Low): a mod's cancel there stands.
    internal static void OnSourceFill(PlayerClientActionEvent e)
    {
        if (e.Canceled || !e.CanCancel || !string.Equals(e.Name, CraftingActionName, StringComparison.Ordinal)) return;
        var options = SourceFill(e.Player.Entity, e.Payload, CanAccessInventories(e.Player), out var station, out var why);
        if (options is null)
        {
            if (why is not null) Kiln.Backend.Current.Log(0, LogLevel.Debug, $"kiln.stations: player {e.Player.Entity}'s fill of station {station}: {why}");
            return;
        }
        e.Cancel();
        var player = e.Player.Entity;
        var at = station;
        _ = Stations.Fill(at, options).ContinueWith(t =>
        {
            var r = t.IsCompletedSuccessfully ? t.Result : null;
            Kiln.Backend.Current.Log(0, r is { Ok: true } ? LogLevel.Info : LogLevel.Warn,
                $"kiln.stations: player {player}'s fill of station {at} ran as Kiln's fill with station outputs: " +
                (r is null ? $"failed ({t.Exception?.GetBaseException().Message})" : $"{r} ({r.FromOutputs} item(s) from station outputs)"));
        }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }

    /// <summary>
    /// The button's fill as Kiln's: the options when the player and the Magic chests do not cover it and the base's
    /// opted-in outputs do; null (with why, null for "not a fill this concerns") when the game should run it as always.
    /// The game's own checks are kept: the player's base (the stock the game would use) must be the station's, and the
    /// player must have CanAccessInventories.
    /// </summary>
    internal static FillOptions? SourceFill(EntityId player, ReadOnlySpan<byte> p, bool? mayAccess, out EntityId station, out string? why)
    {
        why = null;
        station = EntityId.None;
        if (p.Length < 0x41 || p[0x40] != (byte)StationPlayerAction.Fill) return null;
        station = new EntityId(BinaryPrimitives.ReadUInt32LittleEndian(p));
        var recipe = BinaryPrimitives.ReadUInt32LittleEndian(p[4..]);
        var item = BinaryPrimitives.ReadUInt32LittleEndian(p[8..]);
        var amount = BinaryPrimitives.ReadUInt16LittleEndian(p[0x0c..]);
        var water = BinaryPrimitives.ReadUInt32LittleEndian(p[0x10..]);
        // the game's item and water fills, and an empty one, stay the game's
        if (recipe == 0 || item != 0 || water != 0 || amount == 0) return null;
        if (StationOutputKeeper.Count == 0 || station.IsNone) return null;
        if (amount > 1000) return Pass(out why, $"{amount} crafts is more than Kiln's fill takes (1000)");
        if (!SourceFillAllowed(player, station, mayAccess, out why)) return null;
        var choices = new List<CategoryChoice>();
        for (var i = 0; i < 4; i++)
        {
            var cat = BinaryPrimitives.ReadUInt32LittleEndian(p[(0x18 + 8 * i)..]);
            if (cat != 0) choices.Add(new CategoryChoice(cat, p[0x18 + 8 * i + 4]));
        }
        var options = new FillOptions
        {
            Crafts = amount, Mode = FillMode.Add, Exact = true, Recipe = recipe, Choices = choices.Count > 0 ? choices : null, Player = player,
        };
        var plain = Stations.Plan(station, options with { UseStationOutputs = false });
        if (plain.Status == StationActionStatus.Done) return Pass(out why, "the player and the Magic chests cover it");
        var lent = Stations.Plan(station, options);
        if (lent.Status != StationActionStatus.Done) return Pass(out why, $"not even with station outputs: {lent.Reason}");
        return options;
    }

    /// <summary>The button's own rules before any plan: the player may access the base's inventories (the game's
    /// CanAccessInventories), stands in the station's base, and another station of that base is opted in. Shared by the
    /// press (<see cref="SourceFill"/>) and the answers a Kiln client's station screen enables its fill from
    /// (<see cref="FillCrafts"/>).</summary>
    internal static bool SourceFillAllowed(EntityId player, EntityId station, bool? mayAccess, out string? why)
    {
        why = null;
        if (mayAccess is not true) return Pass(out why, mayAccess is null ? "the player's permissions are not known" : "the player may not access inventories") is not null;
        var at = Bases.Of(station);
        var mine = Bases.Of(player);
        if (at is null || mine is null || at.Id != mine.Id) return Pass(out why, "the player stands in another base than the station (or in none)") is not null;
        if (Stations.OutputSources(at.Id).All(s => s == station)) return Pass(out why, "no other station of the base is opted in") is not null;
        return true;
    }

    /// <summary>
    /// GitHub #68, the station screen on a Kiln client: how many more complete crafts the button's fill (this recipe and
    /// these choices, the player's bag first, then the Magic chests, then opted-in station outputs) covers now, by the
    /// same rules <see cref="SourceFill"/> runs the press with. 0: none, or those rules refuse it (no inventory right,
    /// another base, nothing else opted in). The client enables its "+1" and "+10" from it only where the game itself
    /// refused for missing materials; the press is checked again when it comes.
    /// </summary>
    internal static uint FillCrafts(EntityId player, EntityId station, uint recipe, IReadOnlyList<CategoryChoice>? choices, bool? mayAccess)
    {
        if (StationOutputKeeper.Count == 0 || station.IsNone || player.IsNone || recipe == 0) return 0;
        if (!SourceFillAllowed(player, station, mayAccess, out _)) return 0;
        var plan = Stations.Plan(station, new FillOptions
        {
            Crafts = MaxFillCrafts, Mode = FillMode.Add, Exact = false, Recipe = recipe, Choices = choices is { Count: > 0 } ? choices : null,
            Player = player,
        });
        return plan.Status is StationActionStatus.Done or StationActionStatus.Partial && plan.CraftsPlanned > 0 ? (uint)plan.CraftsPlanned : 0;
    }

    /// <summary>The most crafts one fill asks for (the press above it stays the game's).</summary>
    internal const int MaxFillCrafts = 1000;

    private static FillOptions? Pass(out string? why, string text)
    {
        why = text;
        return null;
    }
}

/// <summary>The opt-ins beside the save: kiln.saves' entity attachments under the scope <c>kiln:stations</c> (template and
/// position at the save, as a mod's EntityData), each with the mod that set it. Set again in native when the entity is
/// matched after a restart and that mod is loaded; dropped from native when the station is gone or its mod is not
/// loaded any more (the record stays for the mod's return).</summary>
internal static class StationOutputKeeper
{
    internal const string Scope = "kiln:stations";
    internal const string Key = "output-source";

    internal sealed record Saved([property: JsonPropertyName("mod")] string Mod);

    // on in native this session: entity -> (the mod that set it, kept with the save)
    private static readonly Dictionary<uint, (string? Mod, bool Kept)> On = new();
    private static readonly HashSet<string> Noted = new(StringComparer.Ordinal);
    private static bool _registered;

    internal static int Count => On.Count;

    internal static void Register()
    {
        if (_registered) return;
        var store = ServiceEntityData.Ensure();
        if (store is null) return;
        _registered = true;
        ServiceEntityData.Register(Scope, new ServiceEntityData.Consumer { Loaded = OnLoaded, WorldChanged = OnWorldChanged });
    }

    internal static string Json(Saved s) => JsonSerializer.Serialize(s);

    internal static Saved? Parse(string? json)
    {
        if (json is null) return null;
        try
        {
            return JsonSerializer.Deserialize<Saved>(json) is { Mod.Length: > 0 } s ? s : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    /// <summary>After the native set changed (tick thread, the calling mod's context): records it for the next start.</summary>
    internal static bool Record(EntityId station, bool on, string? mod)
    {
        Register();
        var store = ServiceEntityData.Store;
        if (!on)
        {
            On.Remove(station.Value);
            store?.Set(station, Scope, Key, null);
            return store?.Get(station, Scope, Key) is null;
        }
        var kept = mod is not null && store is not null && store.Set(station, Scope, Key, Json(new Saved(mod)));
        On[station.Value] = (mod, kept);
        Kiln.Backend.Current.Log(0, LogLevel.Info, $"kiln.stations: station {station} lends its outputs as crafting stock (by {mod ?? "Kiln"}" +
                                                  $"{(kept ? ", kept with the save" : ", this session only")})");
        return kept;
    }

    // kiln.saves matched a loaded station to its kept opt-in (tick thread)
    private static void OnLoaded(EntityId entity, IReadOnlyDictionary<string, string> values)
    {
        if (!values.TryGetValue(Key, out var json)) return;
        if (Parse(json) is not { } saved)
        {
            Kiln.Backend.Current.Log(0, LogLevel.Warn, $"kiln.stations: the output-source setting kept for entity {entity} does not read ({json}); left off");
            return;
        }
        if (!ModLive(saved.Mod))
        {
            Note($"kiln.stations: station {entity} was opted in as crafting stock by '{saved.Mod}', which is not loaded: off this session (kept for its return)");
            return;
        }
        if (StationsCore.Backend.SetOutputSource(entity.Value, true) != StationsNative.Ok) return;
        On[entity.Value] = (saved.Mod, true);
        StationsCore.StartPump();
    }

    private static void OnWorldChanged()
    {
        On.Clear();
        Noted.Clear();
    }

    private static bool ModLive(string? id) => id is null || (Mods.Get(id)?.Info is { } info && ModContext.FindLive(info) is not null);

    private static void Note(string text)
    {
        if (Noted.Add(text)) Kiln.Backend.Current.Log(0, LogLevel.Info, text);
    }

    /// <summary>Every 25 steps: a station gone (or its id another entity's now), or its mod unloaded, lends nothing.</summary>
    internal static void Prune()
    {
        if (On.Count == 0) return;
        var store = ServiceEntityData.Store;
        foreach (var (id, (mod, kept)) in On.ToList())
        {
            var e = new EntityId(id);
            string? why = null;
            if (StationsCore.Backend.StationRead(id, out _) != StationsNative.Ok) why = "it is no station any more";
            else if (kept && store is not null && !store.Holds(e, Scope, Key)) why = "the entity is not the one it was set on";
            else if (!ModLive(mod)) why = $"mod '{mod}' is not loaded";
            if (why is null) continue;
            StationsCore.Backend.SetOutputSource(id, false);
            On.Remove(id);
            Note($"kiln.stations: station {e} no longer lends its outputs: {why}");
        }
    }

    /// <summary>Tests: forget everything (a new world).</summary>
    internal static void Reset()
    {
        OnWorldChanged();
        _registered = false;
    }
}
