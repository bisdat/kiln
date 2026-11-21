// SPDX-License-Identifier: MIT
// Base limits (since 0.2.0-dev.31): the Flame Altar count and build-zone sizes this process runs with, and the base rules
// that made them. Read from kiln.base_rules v1 (native/core/abi/services/kiln_base_rules.h, published by kiln.content,
// which composes every mod's base rules into one change of the game's balancing table before the game reads it) and the
// world's progress entity. docs/kiln/services/build.md "Base rules and base limits".
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Kiln;

/// <summary>The build zone of one altar level: the full size of the box an altar of that level lets players build in.</summary>
/// <param name="AltarLevel">The altar level, 1 to 5 (an altar's upgrades; the game's table has a 0 for "no altar").</param>
/// <param name="Size">The full size in metres on each axis (x, y, z). The zone reaches half of it from the altar's centre:
/// the half-extent <see cref="PlayerBase.Half"/> reads back.</param>
public sealed record AltarSize(int AltarLevel, Vector3 Size);

/// <summary>How many Flame Altars the world may have and how large their build zones are in this start: the game's own
/// table and the one the game runs with (after every mod's resource patches and Kiln's composed base rules).</summary>
/// <param name="FlameLevel">The world's Flame level now.</param>
/// <param name="CurrentAltars">Flame Altars the world has now (the game's progress count).</param>
/// <param name="OriginalCount">The game's own count at <paramref name="FlameLevel"/>.</param>
/// <param name="EffectiveCount">The count at <paramref name="FlameLevel"/> in the table the game runs with. The game never
/// allows more than 16, whatever a table says.</param>
/// <param name="OriginalSizes">The game's own build-zone sizes, altar levels 1 to 5.</param>
/// <param name="EffectiveSizes">The build-zone sizes the game runs with, altar levels 1 to 5.</param>
public sealed record BaseLimits(int FlameLevel, int CurrentAltars, int OriginalCount, int EffectiveCount, IReadOnlyList<AltarSize> OriginalSizes,
    IReadOnlyList<AltarSize> EffectiveSizes)
{
    /// <summary>The game's own count at every Flame level (index = Flame level, from 0).</summary>
    public IReadOnlyList<int> OriginalCounts { get; init; } = [];

    /// <summary>The count the game runs with at every Flame level (index = Flame level, from 0).</summary>
    public IReadOnlyList<int> EffectiveCounts { get; init; } = [];

    /// <summary>The base rules in this start's table, as <c>mod:name</c>, in load order (empty without any, and when Kiln
    /// could not write them: the start's log and <c>state/boot.json</c> say why).</summary>
    public IReadOnlyList<string> AppliedRules { get; init; } = [];

    /// <summary>Mods whose own resource patches of the game's balancing table are in this start's table (a mod that
    /// changes the counts or sizes by the game's names, as More Flame Altars does).</summary>
    public IReadOnlyList<string> PatchedBy { get; init; } = [];
}

public static partial class Build
{
    /// <summary>How many Flame Altars the world may have now and how large their build zones are: the game's own numbers
    /// and the ones this start runs with (see <see cref="Kiln.BaseLimits"/>), and the base rules that made them. Base rules
    /// are staged before the game starts (<c>BaseRulesBlock</c> in <c>Mod.OnBoot</c>, or <c>"baseRules"</c> in a blocks file)
    /// and hold until the process ends; this reads them back. Null before the world runs (no progress entity yet).</summary>
    /// <remarks>Server side (a dedicated server, or a game that hosts its world): the world's count and the table its
    /// rules are checked against. A joined game's own table follows the mods it runs itself; it is not read here.</remarks>
    /// <exception cref="KilnUnavailableException"><c>build.base_limits</c> is off (no kiln.base_rules on this build, or kiln.world
    /// is missing).</exception>
    [TickThreadOnly]
    [RequiresCapability(Caps.BaseLimits)]
    public static BaseLimits? BaseLimits
    {
        get
        {
            Rows.Ensure();
            if (!BaseRulesNative.Here || !World.Available) throw ServiceCaps.Unavailable(Caps.BaseLimits);
            Services.RequireTickThread("Build.BaseLimits");
            if (Progress() is not { } p) return null;
            return BaseLimitsReader.From(BaseRulesNative.State(), p.Level, p.Altars);
        }
    }

    [StructLayout(LayoutKind.Sequential, Pack = 2)]
    private struct NetworkServerProgress
    {
        public ushort Level, WaterSources, Altars, HighestAltarLevel, LogicEntities, FactoryBudget;
    }

    private static EntityId _progressEntity;

    // The world's progress entity (the one NetworkServerProgress): its Flame level and altar count. Tick thread.
    private static (int Level, int Altars)? Progress()
    {
        if (_progressEntity.IsNone || !World.Has(_progressEntity, "NetworkServerProgress"))
        {
            var found = World.Find("NetworkServerProgress");
            _progressEntity = found.Count > 0 ? found[0] : EntityId.None;
        }
        if (_progressEntity.IsNone || !World.TryRead<NetworkServerProgress>(_progressEntity, "NetworkServerProgress", out var p)) return null;
        return (p.Level, p.Altars);
    }
}

/// <summary>kiln.base_rules' state JSON as <see cref="BaseLimits"/> (pure: the offline tests feed it).</summary>
internal static class BaseLimitsReader
{
    internal static BaseLimits? From(string? json, int flameLevel, int currentAltars)
    {
        if (json is null) return null;
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        if (!root.TryGetProperty("table", out var table) || table.ValueKind != JsonValueKind.True) return null;
        var originalCounts = Counts(root.GetProperty("original"));
        var effectiveCounts = Counts(root.GetProperty("effective"));
        static int At(IReadOnlyList<int> counts, int level) => counts.Count == 0 ? 0 : counts[Math.Clamp(level, 0, counts.Count - 1)];
        var applied = new List<string>();
        if (root.TryGetProperty("rules", out var rules) && rules.ValueKind == JsonValueKind.Array)
            foreach (var r in rules.EnumerateArray())
                if (r.TryGetProperty("applied", out var a) && a.ValueKind == JsonValueKind.True)
                    applied.Add($"{r.GetProperty("mod").GetString()}:{r.GetProperty("name").GetString()}");
        var patchedBy = new List<string>();
        if (root.TryGetProperty("patchedBy", out var pb) && pb.ValueKind == JsonValueKind.Array)
            foreach (var m in pb.EnumerateArray())
                if (m.GetString() is { } s) patchedBy.Add(s);
        return new BaseLimits(flameLevel, currentAltars, At(originalCounts, flameLevel), At(effectiveCounts, flameLevel),
            Sizes(root.GetProperty("original")), Sizes(root.GetProperty("effective")))
        {
            OriginalCounts = originalCounts, EffectiveCounts = effectiveCounts, AppliedRules = applied, PatchedBy = patchedBy,
        };
    }

    private static int[] Counts(JsonElement lists) =>
        lists.TryGetProperty("altars", out var a) && a.ValueKind == JsonValueKind.Array ? a.EnumerateArray().Select(x => (int)Math.Round(x.GetDouble())).ToArray() : [];

    // altar levels 1..5 (the table's level 0 is "no altar")
    private static AltarSize[] Sizes(JsonElement lists)
    {
        if (!lists.TryGetProperty("sizes", out var s) || s.ValueKind != JsonValueKind.Array) return [];
        var list = new List<AltarSize>();
        var level = 0;
        foreach (var e in s.EnumerateArray())
        {
            if (level > 0 && e.ValueKind == JsonValueKind.Array && e.GetArrayLength() == 3)
                list.Add(new AltarSize(level, new Vector3(e[0].GetSingle(), e[1].GetSingle(), e[2].GetSingle())));
            level++;
        }
        return [.. list];
    }
}

/// <summary>Mirror of native/core/abi/services/kiln_base_rules.h (kiln.base_rules v1).</summary>
internal static unsafe class BaseRulesNative
{
    public const int Ok = 0, BufferTooSmall = 5;

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<byte*, nuint, nuint*, int> State;
    }

    private static Table* _table;
    private static bool _checked;

    public static Table* T
    {
        get
        {
            if (_table != null || _checked) return _table;
            _checked = true;
            _table = Services.Get<Table>("kiln.base_rules", 1);
            if (_table != null && _table->Header.Size < sizeof(Table)) _table = null;
            return _table;
        }
    }

    public static bool Here => T != null;

    /// <summary>The state JSON; null when the table is not here or does not answer.</summary>
    public static string? State()
    {
        var t = T;
        if (t == null) return null;
        var cap = 16384;
        for (var attempt = 0; attempt < 3; attempt++)
        {
            var buf = new byte[cap];
            nuint len = 0;
            int rc;
            fixed (byte* b = buf) rc = t->State(b, (nuint)buf.Length, &len);
            if (rc == Ok) return Encoding.UTF8.GetString(buf, 0, (int)len);
            if (rc != BufferTooSmall) return null;
            cap = (int)len + 1;
        }
        return null;
    }
}
