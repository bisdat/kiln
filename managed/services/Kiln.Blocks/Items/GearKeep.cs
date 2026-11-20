// SPDX-License-Identifier: MIT
// Keeping one item's per-instance values through saves and restarts (the equipment wave, package P7): the power and armour
// multipliers (Items.SetPower, Items.SetArmor), a base level outside the item type's range (Items.SetLevel with AnyLevel)
// and a max durability (Items.SetDurabilityMax). The game itself keeps none of them: the scales live in kiln.items' table
// of this server world, a load sets the level back into the type's range and computes max durability again
// (docs/re/items.md section 20). Kiln keeps them as item data of its own (owner "kiln-gear", key "values"), so they follow
// the item through moves, saves, restarts and a restored backup like any item data, and a pass about once a second writes
// them again wherever the item differs (after a load, after a world restart, after a rarity change that made the game
// compute max durability again). Server-authoritative: the pass runs on the server only.
// The item data is synced to players near the item (Sync Nearby): a Kiln client's tooltip values provider (ui.tooltip.values)
// shows the item's main stat number times its multiplier; a vanilla client shows the type's number.
using System.Text.Json;

namespace Kiln;

/// <summary>The stored value: what Kiln keeps for one item. Unknown fields are ignored; a stored value that does not read
/// is treated as none (and left as it is).</summary>
internal sealed class GearKeptData
{
    public double? Power { get; set; }
    public double? Armor { get; set; }
    public int? BaseLevel { get; set; }
    public int? DurabilityMax { get; set; }

    /// <summary>The multiplier a Kiln client's tooltip applies to the item's main stat number: Power for weapons and
    /// ammunition, Armor for anything else (set on the server, where the item type is known).</summary>
    public double? Shown { get; set; }
}

/// <summary>Kiln.Blocks starts the keeping at the first service tick (EventsTick's AutoStart).</summary>
internal static class GearKeepStartup
{
    static GearKeepStartup() => GearKeepRuntime.Start();
}

internal static class GearKeepRuntime
{
    internal const string DataMod = "kiln-gear";
    internal const string DataKey = "values";
    internal const ulong PassEverySteps = 25;   // about once a second
    internal const ulong RetrySteps = 50;       // a write of one value of one item is not asked again sooner

    private static readonly object Gate = new();
    private static bool _started, _clientRegistered;
    private static bool? _used;  // the marker: values were kept on this install once (the pass starts item data only then)
    private static ModContext? _owner;
    private static IDisposable? _tick;
    private static UiValueRegistration? _tooltip;

    /// <summary>What the pass needs from the game, behind one interface (managed/tests/Kiln.Blocks.Tests/GearKeepTests.cs).</summary>
    internal interface IGame
    {
        IReadOnlyList<(ItemInstanceRef Item, GearKeptData Data)> Kept();
        GearState? Gear(ItemInstanceRef item);
        (double Power, double Armor)? Scales(ItemInstanceRef item);
        bool SetScale(ItemInstanceRef item, uint which, double scale);
        bool WriteLevel(ItemInstanceRef item, int level);
        bool WriteDurabilityMax(ItemInstanceRef item, int max);
    }

    internal static IGame Game { get; set; } = new RealGame();
    private static readonly Dictionary<(ItemInstanceRef Item, int Kind), ulong> Asked = new();

    internal static void Start()
    {
        lock (Gate)
        {
            if (_started) return;
            _started = true;
        }
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own tick, not a mod's
        try
        {
            Items.RegisterKeeper(new Keeper());
            _tick = ServiceTickAccess.Add("kiln gear values", Tick);
            if (Client.IsClient) Ui.Screens.Changed += (_, _) => RegisterTooltip();
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln gear values: not started: {e.Message}");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    private static ModContext Owner => _owner ??= HostAccess.CreateContext(
        new ModInfo(DataMod, "Kiln gear values", "1.0.0", Path.Combine(KilnDir(), "host")), 0, Path.Combine(KilnDir(), "data", DataMod));

    private static string KilnDir()
    {
        var host = Path.GetDirectoryName(typeof(GearKeepRuntime).Assembly.Location) ?? ".";
        return Path.GetDirectoryName(host) ?? host;
    }

    /// <summary>The item data key, Kiln's own (asked for at every use, as affixes do: a store that started over gets it
    /// declared again). Tick thread.</summary>
    internal static InstanceKey<GearKeptData> Key
    {
        get
        {
            var outer = ModContext.CurrentMod;
            ModContext.CurrentMod = Owner;
            try
            {
                return InstanceData.Key<GearKeptData>(DataKey, new InstanceKeyOptions { Sync = InstanceSync.Nearby });
            }
            finally
            {
                ModContext.CurrentMod = outer;
            }
        }
    }

    // ---- what Kiln.Items asks (IGearKeeper) ----

    internal sealed class Keeper : IGearKeeper
    {
        public GearKept? Of(ItemInstanceRef item) => item.TryGet(Key, out var d) ? ToKept(d) : null;

        public bool Store(ItemInstanceRef item, GearKept? kept)
        {
            if (kept is null)
            {
                item.Remove(Key);
                return true;
            }
            item.Set(Key, ToData(kept, ShownOf(item.Item, kept)));
            MarkUsed();
            return true;
        }
    }

    internal static GearKept ToKept(GearKeptData d) =>
        new(Valid(d.Power) ?? 1, Valid(d.Armor) ?? 1, d.BaseLevel is >= 1 and <= 255 ? d.BaseLevel : null,
            d.DurabilityMax is >= 1 and <= 1000000 ? d.DurabilityMax : null);

    internal static GearKeptData ToData(GearKept k, double? shown) => new()
    {
        Power = k.Power != 1 ? k.Power : null, Armor = k.Armor != 1 ? k.Armor : null, BaseLevel = k.BaseLevel, DurabilityMax = k.DurabilityMax,
        Shown = shown is { } s && s != 1 ? s : null,
    };

    private static double? Valid(double? v) => v is { } x && double.IsFinite(x) && x >= Items.MinScale && x <= Items.MaxScale ? x : null;

    /// <summary>Which multiplier a tooltip shows for the item's type: its power for weapons and ammunition, else its armour.</summary>
    internal static double ShownOf(ItemId type, GearKept k)
    {
        ItemCategory? category = null;
        try
        {
            category = Items.Get(type)?.Category;
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException)
        {
        }
        return category is ItemCategory.Weapons or ItemCategory.Ammunition ? k.Power : category is null && k.Armor == 1 ? k.Power : k.Armor;
    }

    // ---- the marker ----
    // The pass reads item data, which starts Kiln.Saves' item data for the world. A server that never kept a value has no
    // reason to start it, so the pass runs only once this install kept a value (<kiln>/data/kiln-gear/used).

    private static string MarkerPath => Path.Combine(KilnDir(), "data", DataMod, "used");

    internal static bool Used
    {
        get
        {
            if (_used is { } u) return u;
            try
            {
                _used = File.Exists(MarkerPath);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                _used = false;
            }
            return _used.Value;
        }
        set => _used = value;
    }

    private static void MarkUsed()
    {
        if (Used) return;
        _used = true;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(MarkerPath)!);
            File.WriteAllText(MarkerPath, "Kiln keeps per-item gear values here: the server checks item data for them after each load.\n");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln gear values: the marker {MarkerPath} could not be written ({e.Message}): " +
                                                  "values kept now are written again after a load only when another mod's item data starts first");
        }
    }

    // ---- the pass (server) ----

    private static void Tick(ulong step)
    {
        if (!Lifecycle.IsServerRunning)
        {
            if (Asked.Count > 0) Asked.Clear();
            return;
        }
        if (step % PassEverySteps != 0 || !Used) return;
        try
        {
            Pass(Game, step);
        }
        catch (KilnUnavailableException)
        {
            // item data or gear off now: nothing to keep; the next pass asks again
        }
    }

    /// <summary>Makes every item with kept values equal to them again where it differs (one write per value, not asked
    /// again for <see cref="RetrySteps"/>). Returns how many writes it asked for.</summary>
    internal static int Pass(IGame game, ulong step)
    {
        var asked = 0;
        foreach (var (item, data) in game.Kept())
        {
            var k = ToKept(data);
            if (k.IsEmpty) continue;
            var scales = game.Scales(item);
            if (scales is null) continue;  // gone, or its id names another item now
            if (scales.Value.Power != k.Power && Ask(item, 0, step) && game.SetScale(item, ItemsNativeScale.Power, k.Power)) asked++;
            if (scales.Value.Armor != k.Armor && Ask(item, 1, step) && game.SetScale(item, ItemsNativeScale.Armor, k.Armor)) asked++;
            if (k.BaseLevel is null && k.DurabilityMax is null) continue;
            var g = game.Gear(item);
            if (g is null) continue;
            if (k.BaseLevel is { } b && g.Level > 0 && g.BaseLevel != b && b + g.Unlocked <= 255 && Ask(item, 2, step) && game.WriteLevel(item, b + g.Unlocked)) asked++;
            if (k.DurabilityMax is { } m && g.DurabilityMax is { } have && have != m && Ask(item, 3, step) && game.WriteDurabilityMax(item, m)) asked++;
        }
        if (Asked.Count > 4096)
            foreach (var key in Asked.Where(p => step - p.Value > RetrySteps).Select(p => p.Key).ToList()) Asked.Remove(key);
        return asked;
    }

    private static bool Ask(ItemInstanceRef item, int kind, ulong step)
    {
        if (Asked.TryGetValue((item, kind), out var last) && step - last < RetrySteps) return false;
        Asked[(item, kind)] = step;
        return true;
    }

    internal static void ResetForTests()
    {
        Asked.Clear();
        Game = new RealGame();
        _used = null;
    }

    private sealed class RealGame : IGame
    {
        public IReadOnlyList<(ItemInstanceRef Item, GearKeptData Data)> Kept() => InstanceData.All(Key);

        public GearState? Gear(ItemInstanceRef item) => Items.GearReadAvailable ? Items.GearOf(item) : null;

        public (double Power, double Armor)? Scales(ItemInstanceRef item) => Items.ValuesOf(item) is { } v ? (v.Power, v.Armor) : null;

        public bool SetScale(ItemInstanceRef item, uint which, double scale)
        {
            var row = which == ItemsNativeScale.Power ? "items.gear.power" : "items.gear.armor";
            if (!Capabilities.Is(row)) return false;
            return Items.ReapplyScale(item, which, scale);
        }

        public bool WriteLevel(ItemInstanceRef item, int level)
        {
            if (!Items.GearWriteAvailable) return false;
            _ = Items.ReapplyLevel(item, level);
            return true;
        }

        public bool WriteDurabilityMax(ItemInstanceRef item, int max)
        {
            if (!Items.GearWriteAvailable) return false;
            _ = Items.ReapplyDurabilityMax(item, max);
            return true;
        }
    }

    // ---- the tooltip (Kiln clients) ----

    /// <summary>Registers the tooltip values provider once ui.tooltip.values is on (tried at screen changes: no work in
    /// other frames).</summary>
    internal static void RegisterTooltip()
    {
        if (_clientRegistered || !Capabilities.Is(Ui.Tooltip.ValuesCapability)) return;
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;
        try
        {
            var r = Ui.Tooltip.AddValues(Provide);
            if (r.Ok)
            {
                _tooltip = r;
                _clientRegistered = true;
            }
        }
        catch (KilnUnavailableException)
        {
            // not now; the next screen change tries again
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    private static string? _lastJson;
    private static float _lastFactor = 1f;

    /// <summary>The factor of the item's main stat number: the kept multiplier the server synced for that item entity.</summary>
    internal static UiTooltipScale Provide(in UiValueContext context, in UiItemView item)
    {
        if (item.ServerEntity.IsNone) return UiTooltipScale.None;
        string? json;
        try
        {
            json = InstanceData.Synced(item.ServerEntity)?.Json(DataMod, DataKey);
        }
        catch (KilnUnavailableException)
        {
            return UiTooltipScale.None;
        }
        return json is null ? UiTooltipScale.None : UiTooltipScale.By(FactorOf(json));
    }

    /// <summary>The shown multiplier of a synced value (cached for the last text: a detail view asks every frame).</summary>
    internal static float FactorOf(string json)
    {
        if (json == _lastJson) return _lastFactor;
        float factor = 1f;
        try
        {
            var d = JsonSerializer.Deserialize<GearKeptData>(json);
            if (d?.Shown is { } s && double.IsFinite(s) && s >= Items.MinScale && s <= Items.MaxScale) factor = (float)s;
        }
        catch (JsonException)
        {
        }
        _lastJson = json;
        _lastFactor = factor;
        return factor;
    }
}

/// <summary>kiln.items' scale flags (KILN_ITEMS_SCALE_*), as the pass names them.</summary>
internal static class ItemsNativeScale
{
    internal const uint Power = 0x01, Armor = 0x02;
}

