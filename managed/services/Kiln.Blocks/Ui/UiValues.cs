// SPDX-License-Identifier: MIT
// The typed names of UI blocks (dossier 09 §4.3) and the one context a refresh reads them through (§4.4 RefreshContext).
//   state.<key>, state.<key>/count     the window's state (UiState.cs)
//   mod.<id>                           the mod's own last publication (UiDataRegistry.cs)
//   world.local.*, world.server.*      kiln.ui.snapshot's publication (UiWorldSnapshot.cs): no world read here
//   stats.<mod>/<id>[/level|/revision|/available]   05b's synced copy through ModValuesStatSource; a foreign stat needs an import
//   row.<field>, node.<field>          a table's row, a tree's node, inside its cells / node rows
// A context captures each source once (one world publication, one stats copy, one state snapshot), so every row of one
// refresh sees the same values, and it reads nothing that is not already an immutable copy: it may run on the render thread.
using System.Collections.Immutable;
using System.Globalization;
using System.Text.RegularExpressions;

namespace Kiln;

/// <summary>Where stat bindings come from: 05b's internal stat source in a game; tests replace it.</summary>
internal static class UiStats
{
    public static Func<ModValueKey, ModValueDefinition?> Metadata = static key => ModValuesStatSource.Metadata(key);
    public static Func<string?, ModValueKey, ModValueReadResult> Read = static (reader, key) => ModValuesStatSource.Read(reader, key);

    private static readonly object Gate = new();
    private static readonly Dictionary<string, HashSet<string>> Imports = new(StringComparer.Ordinal);

    /// <summary>Declares that <paramref name="mod"/>'s blocks show another mod's stat (Blocks.ImportStat, statImports).</summary>
    public static Action Import(string mod, ModValueKey key)
    {
        var name = $"{key.Mod}:{key.Id}";
        lock (Gate)
        {
            if (!Imports.TryGetValue(mod, out var set)) Imports[mod] = set = new(StringComparer.Ordinal);
            set.Add(name);
        }
        return () => { lock (Gate) if (Imports.TryGetValue(mod, out var set)) set.Remove(name); };
    }

    public static bool Imported(string mod, ModValueKey key)
    {
        lock (Gate) return Imports.TryGetValue(mod, out var set) && set.Contains($"{key.Mod}:{key.Id}");
    }

    public static void Forget(string mod)
    {
        lock (Gate) Imports.Remove(mod);
    }

    internal static void ResetForTests()
    {
        lock (Gate) Imports.Clear();
        Metadata = static key => ModValuesStatSource.Metadata(key);
        Read = static (reader, key) => ModValuesStatSource.Read(reader, key);
    }
}

internal static partial class UiTypedNames
{
    /// <summary>Ids of rows, state keys and data sources.</summary>
    [GeneratedRegex("^[a-z][a-z0-9_-]{0,63}$", RegexOptions.CultureInvariant)]
    internal static partial Regex IdPattern();

    internal static bool IsId(string? s) => s != null && IdPattern().IsMatch(s);

    /// <summary>The built-in fields of world.local.inventory's rows.</summary>
    internal static readonly IReadOnlyList<UiField> InventoryFields =
    [
        new("key", UiScalarType.String), new("item", UiScalarType.String), new("itemId", UiScalarType.UInt64), new("count", UiScalarType.UInt64),
        new("pide", UiScalarType.UInt64), new("inventoryPage", UiScalarType.UInt64), new("slotIndex", UiScalarType.UInt64),
    ];

    /// <summary>A tree node's built-in fields (node.*).</summary>
    internal static readonly IReadOnlyList<UiField> NodeBuiltIns =
        [new("key", UiScalarType.String), new("parent", UiScalarType.String), new("label", UiScalarType.String), new("depth", UiScalarType.UInt64)];

    private static readonly Dictionary<string, (UiValueType Type, UiWorldFields Fields)> Local = new(StringComparer.Ordinal)
    {
        ["available"] = (UiValueType.Bool, UiWorldFields.None),
        ["player"] = (UiValueType.UInt64, UiWorldFields.None),
        ["session"] = (UiValueType.UInt64, UiWorldFields.None),
        ["step"] = (UiValueType.UInt64, UiWorldFields.None),
        ["health"] = (UiValueType.UInt64, UiWorldFields.LocalVitals),
        ["healthMax"] = (UiValueType.UInt64, UiWorldFields.LocalVitals),
        ["stamina"] = (UiValueType.UInt64, UiWorldFields.LocalVitals),
        ["staminaMax"] = (UiValueType.UInt64, UiWorldFields.LocalVitals),
        ["mana"] = (UiValueType.UInt64, UiWorldFields.LocalVitals),
        ["manaMax"] = (UiValueType.UInt64, UiWorldFields.LocalVitals),
        ["position.x"] = (UiValueType.Decimal, UiWorldFields.LocalPosition),
        ["position.y"] = (UiValueType.Decimal, UiWorldFields.LocalPosition),
        ["position.z"] = (UiValueType.Decimal, UiWorldFields.LocalPosition),
        ["inventory"] = (UiValueType.Rows, UiWorldFields.LocalInventory),
        ["inventoryCount"] = (UiValueType.UInt64, UiWorldFields.LocalInventory),
    };

    public static UiValueType TypeOf(UiScalarType t) => t switch
    {
        UiScalarType.Bool => UiValueType.Bool,
        UiScalarType.Int64 => UiValueType.Int64,
        UiScalarType.UInt64 => UiValueType.UInt64,
        UiScalarType.Decimal => UiValueType.Decimal,
        UiScalarType.Float64 => UiValueType.Number,
        _ => UiValueType.Text,
    };

    public static UiValueType TypeOf(UiStateDefinition d) => d switch
    {
        UiBoolState => UiValueType.Bool,
        UiInt64State => UiValueType.Int64,
        UiDecimalState => UiValueType.Decimal,
        UiStringState => UiValueType.Text,
        _ => UiValueType.StringSet,
    };

    /// <summary>True with the binding when <paramref name="spec"/> is a typed name; false with a problem when it starts like
    /// one but is wrong; false with no problem when it is no typed name at all.</summary>
    public static bool TryResolve(UiScope scope, string spec, out UiBinding? binding, out string? problem)
    {
        binding = null;
        problem = null;
        if (spec.StartsWith("state.", StringComparison.Ordinal)) return State(scope, spec, out binding, out problem);
        if (spec.StartsWith("mod.", StringComparison.Ordinal)) return ModData(scope, spec, out binding, out problem);
        if (spec.StartsWith("world.local.", StringComparison.Ordinal))
        {
            var name = spec[12..];
            if (!Local.TryGetValue(name, out var t))
            {
                if (name.EndsWith(".available", StringComparison.Ordinal)) return false;
                problem = $"\"{spec}\" is not a value of this game's player; there are: {string.Join(", ", Local.Keys.Select(k => "world.local." + k))}";
                return false;
            }
            binding = new UiBinding(spec, t.Type) { Source = UiSource.WorldLocal, Key = name, World = t.Fields, Fields = name == "inventory" ? InventoryFields : null };
            return true;
        }
        if (spec.StartsWith("world.server.", StringComparison.Ordinal))
        {
            var name = spec[13..];
            if (name is "available") { binding = new UiBinding(spec, UiValueType.Bool) { Source = UiSource.WorldServer, Key = name }; return true; }
            if (name is "hours") { binding = new UiBinding(spec, UiValueType.Decimal) { Source = UiSource.WorldServer, Key = name, World = UiWorldFields.HostTime }; return true; }
            if (name.EndsWith(".available", StringComparison.Ordinal)) return false;
            problem = $"\"{spec}\" is not a value of the hosted world; there are: world.server.available, world.server.hours";
            return false;
        }
        if (spec.StartsWith("stats.", StringComparison.Ordinal)) return Stat(scope, spec, out binding, out problem);
        if (spec.StartsWith("row.", StringComparison.Ordinal))
        {
            var f = spec[4..];
            if (scope.RowFields == null) { problem = $"\"{spec}\": row.* is only inside a table's cells"; return false; }
            var field = scope.RowFields.FirstOrDefault(x => x.Id == f);
            if (field == null)
            {
                if (f.EndsWith(".available", StringComparison.Ordinal)) return false;
                problem = $"\"{spec}\": the table's rows have the fields {string.Join(", ", scope.RowFields.Select(x => "row." + x.Id))}";
                return false;
            }
            binding = new UiBinding(spec, TypeOf(field.Type)) { Source = UiSource.Row, Key = f };
            return true;
        }
        if (spec.StartsWith("node.", StringComparison.Ordinal))
        {
            var f = spec[5..];
            if (scope.NodeFields == null) { problem = $"\"{spec}\": node.* is only inside a tree's node rows"; return false; }
            var field = NodeBuiltIns.Concat(scope.NodeFields).FirstOrDefault(x => x.Id == f);
            if (field == null)
            {
                if (f.EndsWith(".available", StringComparison.Ordinal)) return false;
                problem = $"\"{spec}\": the tree's nodes have the fields {string.Join(", ", NodeBuiltIns.Concat(scope.NodeFields).Select(x => "node." + x.Id))}";
                return false;
            }
            binding = new UiBinding(spec, TypeOf(field.Type)) { Source = UiSource.Node, Key = f };
            return true;
        }
        return false;
    }

    private static bool State(UiScope scope, string spec, out UiBinding? binding, out string? problem)
    {
        binding = null;
        problem = null;
        var rest = spec[6..];
        var count = rest.EndsWith("/count", StringComparison.Ordinal);
        var key = count ? rest[..^6] : rest;
        if (scope.State == null)
        {
            problem = $"\"{spec}\": state.* is a window's own state, and this is not in a window that declares state (a HUD line or a key reads it through C#: BlockUi.State)";
            return false;
        }
        if (!scope.State.TryGetValue(key, out var def))
        {
            if (key.EndsWith(".available", StringComparison.Ordinal)) return false;
            problem = scope.State.Count == 0 ? $"\"{spec}\": the window declares no state" : $"\"{spec}\": the window's state has {string.Join(", ", scope.State.Keys.Select(k => "state." + k))}";
            return false;
        }
        if (count)
        {
            if (def is not UiStringSetState) { problem = $"\"{spec}\": /count is for a stringSet entry; state.{key} is not one"; return false; }
            binding = new UiBinding(spec, UiValueType.UInt64) { Source = UiSource.StateCount, Key = key, State = def };
            return true;
        }
        binding = new UiBinding(spec, TypeOf(def)) { Source = UiSource.State, Key = key, State = def };
        return true;
    }

    private static bool ModData(UiScope scope, string spec, out UiBinding? binding, out string? problem)
    {
        binding = null;
        problem = null;
        var id = spec[4..];
        if (UiDataRegistry.SchemaOf(scope.Mod, id) is not { } schema)
        {
            if (id.EndsWith(".available", StringComparison.Ordinal)) return false;
            problem = $"\"{spec}\": mod {scope.Mod} has no data source \"{id}\": define it with BlockUi.Bindings.Define(\"{id}\", schema) before Blocks.Define (a mod reads only its own sources)";
            return false;
        }
        var type = schema.Shape switch { UiDataShape.Rows => UiValueType.Rows, UiDataShape.Tree => UiValueType.Tree, _ => TypeOf(schema.ScalarType ?? UiScalarType.String) };
        binding = new UiBinding(spec, type) { Source = UiSource.Mod, Key = id, Fields = schema.Fields ?? [] };
        return true;
    }

    private static bool Stat(UiScope scope, string spec, out UiBinding? binding, out string? problem)
    {
        binding = null;
        problem = null;
        var parts = spec[6..].Split('/');
        if (parts.Length is < 2 or > 3 || parts[0].Length == 0 || parts[1].Length == 0)
        {
            if (spec.EndsWith(".available", StringComparison.Ordinal)) return false;
            problem = $"\"{spec}\": a stat is stats.<mod>/<id>, with /level, /revision or /available after it";
            return false;
        }
        var part = parts.Length == 3 ? parts[2] : "value";
        if (part is not ("value" or "level" or "revision" or "available"))
        {
            problem = $"\"{spec}\": after a stat comes /level, /revision or /available";
            return false;
        }
        var key = new ModValueKey(parts[0], parts[1]);
        var own = parts[0] == scope.Mod;
        if (own)
        {
            if (UiStats.Metadata(key) == null)
            {
                problem = $"\"{spec}\": mod {scope.Mod} has no stat \"{parts[1]}\": describe it (mod-values/*.json or ModValues.Describe) before the block is defined";
                return false;
            }
        }
        else if (!UiStats.Imported(scope.Mod, key))
        {
            problem = $"\"{spec}\": another mod's stat needs an import first: Blocks.ImportStat(ModValues.Key(\"{parts[0]}:{parts[1]}\")), or \"statImports\": [\"{parts[0]}:{parts[1]}\"] in the block file";
            return false;
        }
        var type = part switch { "value" => UiValueType.Decimal, "available" => UiValueType.Bool, _ => UiValueType.UInt64 };
        binding = new UiBinding(spec, type) { Source = UiSource.Stats, Key = $"{key.Mod}:{key.Id}", Part = part };
        return true;
    }

    /// <summary>A scalar of a data source or a static tree as the UI holds it.</summary>
    public static object Box(UiScalar s) => s switch
    {
        UiScalar.Bool b => b.Value,
        UiScalar.Int64 l => l.Value,
        UiScalar.UInt64 u => u.Value,
        UiScalar.Decimal d => d.Value,
        UiScalar.Float64 f => f.Value,
        UiScalar.String t => t.Value,
        _ => throw new ArgumentException("an unknown scalar"),
    };

    /// <summary>A state value as the UI holds it.</summary>
    public static object Box(UiStateValue v) => v switch
    {
        UiStateValue.Bool b => b.Value,
        UiStateValue.Int64 l => l.Value,
        UiStateValue.Decimal d => d.Value,
        UiStateValue.String s => s.Value,
        UiStateValue.StringSet s => s.Value is ImmutableArray<string> a ? a : s.Value.ToImmutableArray(),
        _ => throw new ArgumentException("an unknown state value"),
    };
}

/// <summary>One refresh's view of every source: captured once, read many times; render-thread safe.</summary>
internal sealed class UiRefreshContext
{
    private readonly UiScope _scope;
    private readonly Shared _shared;

    private sealed class Shared
    {
        public UiWorldReadResult? World;
        public Dictionary<string, object?> Cache = new(StringComparer.Ordinal);
    }

    public UiRefreshContext(UiScope scope, IReadOnlyDictionary<string, UiStateValue>? state = null, Func<UiBinding, object?>? pending = null)
    {
        _scope = scope;
        _shared = new Shared();
        State = state;
        Pending = pending;
    }

    private UiRefreshContext(UiRefreshContext from, UiRowEntry? row, UiTreeEntry? node)
    {
        _scope = from._scope;
        _shared = from._shared;
        State = from.State;
        Pending = from.Pending;
        Row = row ?? from.Row;
        Node = node ?? from.Node;
    }

    public IReadOnlyDictionary<string, UiStateValue>? State { get; }
    /// <summary>A config edit not written yet (the window shows it at once).</summary>
    public Func<UiBinding, object?>? Pending { get; }
    public UiRowEntry? Row { get; }
    public UiTreeEntry? Node { get; }

    public UiRefreshContext ForRow(UiRowEntry row) => new(this, row, null);
    public UiRefreshContext ForNode(UiTreeEntry node) => new(this, null, node);

    /// <summary>The world publication this refresh uses (read once).</summary>
    public UiWorldReadResult World => _shared.World ??= SafeWorld();

    private static UiWorldReadResult SafeWorld()
    {
        try { return UiWorldSource.Read(); }
        catch (Exception e) when (e is not OutOfMemoryException) { return new UiWorldReadResult(false, e.Message, null); }
    }

    public object? Value(UiBinding b)
    {
        switch (b.Source)
        {
            case UiSource.Row:
                return Row is { } r && r.Values.TryGetValue(b.Key!, out var rv) ? rv : new UiUnavailable("NoRow");
            case UiSource.Node:
                if (Node is not { } n) return new UiUnavailable("NoNode");
                return b.Key switch
                {
                    "key" => n.Key,
                    "parent" => n.Parent ?? "",
                    "label" => n.Label.For(UiRuntime.Backend.Language),
                    "depth" => (ulong)n.Depth,
                    _ => n.Values.TryGetValue(b.Key!, out var nv) ? nv : new UiUnavailable("NoField"),
                };
            case UiSource.Available:
                return Value(b.Of!) is not (UiUnavailable or null);
        }
        if (_shared.Cache.TryGetValue(b.Name, out var cached)) return cached;
        var v = Read(b);
        _shared.Cache[b.Name] = v;
        return v;
    }

    private object? Read(UiBinding b)
    {
        switch (b.Source)
        {
            case UiSource.Game:
            case UiSource.Config:
                return Pending?.Invoke(b) ?? UiBindings.Read(b);
            case UiSource.State:
                return State != null && State.TryGetValue(b.Key!, out var s) ? UiTypedNames.Box(s) : new UiUnavailable("NoState");
            case UiSource.StateCount:
                return State != null && State.TryGetValue(b.Key!, out var set) && set is UiStateValue.StringSet ss ? (ulong)ss.Value.Count : new UiUnavailable("NoState");
            case UiSource.Mod:
                return UiDataRegistry.Latest(_scope.Mod, b.Key!) ?? new UiUnavailable("NotPublished");
            case UiSource.WorldLocal:
                return Local(b);
            case UiSource.WorldServer:
                if (b.Key == "available")
                {
                    try { return UiWorldSource.Hosting(); }
                    catch (Exception e) when (e is not OutOfMemoryException) { return false; }
                }
                return Field(UiWorldFields.HostTime, w => (object)w.ServerHours);
            case UiSource.Stats:
                return Stat(b);
            default:
                return new UiUnavailable("Unknown");
        }
    }

    private object Local(UiBinding b)
    {
        var w = World;
        switch (b.Key)
        {
            case "available": return w is { Ok: true, Snapshot: { } s0 } && s0.Local.Session != 0;
            case "player": return w.Snapshot is { } s1 ? (ulong)s1.LocalPlayer : Why(w);
            case "session": return w.Snapshot is { } s2 ? s2.Local.Session : Why(w);
            case "step": return w.Snapshot is { } s3 ? s3.Local.Step : Why(w);
            case "health": return Field(UiWorldFields.LocalVitals, s => (ulong)s.Health);
            case "healthMax": return Field(UiWorldFields.LocalVitals, s => (ulong)s.HealthMax);
            case "stamina": return Field(UiWorldFields.LocalVitals, s => (ulong)s.Stamina);
            case "staminaMax": return Field(UiWorldFields.LocalVitals, s => (ulong)s.StaminaMax);
            case "mana": return Field(UiWorldFields.LocalVitals, s => (ulong)s.Mana);
            case "manaMax": return Field(UiWorldFields.LocalVitals, s => (ulong)s.ManaMax);
            case "position.x": return Field(UiWorldFields.LocalPosition, s => Metres(s.XFixed));
            case "position.y": return Field(UiWorldFields.LocalPosition, s => Metres(s.YFixed));
            case "position.z": return Field(UiWorldFields.LocalPosition, s => Metres(s.ZFixed));
            case "inventoryCount": return Field(UiWorldFields.LocalInventory, s => (ulong)s.Inventory.Count);
            case "inventory": return Field(UiWorldFields.LocalInventory, s => Inventory(s));
            default: return new UiUnavailable("Unknown");
        }
    }

    /// <summary>32.32 fixed metres as a decimal rounded to six places, half to even.</summary>
    internal static decimal Metres(long raw) => Math.Round((decimal)raw / 4294967296m, 6, MidpointRounding.ToEven);

    private static UiRowsValue Inventory(UiWorldSnapshot s)
    {
        var rows = ImmutableArray.CreateBuilder<UiRowEntry>(s.Inventory.Count);
        foreach (var i in s.Inventory)
            rows.Add(new UiRowEntry(i.Key, ImmutableDictionary.CreateRange(StringComparer.Ordinal, new KeyValuePair<string, object>[]
            {
                new("key", i.Key), new("item", "0x" + i.Item.ToString("x8", CultureInfo.InvariantCulture)), new("itemId", (ulong)i.Item),
                new("count", (ulong)i.Count), new("pide", (ulong)i.Pide), new("inventoryPage", (ulong)i.InventoryPage), new("slotIndex", (ulong)i.SlotIndex),
            })));
        return new UiRowsValue(rows.MoveToImmutable(), s.Publication);
    }

    private object Field(UiWorldFields field, Func<UiWorldSnapshot, object> read)
    {
        var w = World;
        if (w.Snapshot is not { } s) return Why(w);
        if (s.Has(field)) return read(s);
        var bit = System.Numerics.BitOperations.TrailingZeroCount((uint)field);
        var reason = bit < s.UnavailableReasons.Count ? s.UnavailableReasons[bit] : null;
        return new UiUnavailable(reason ?? "NoSnapshot");
    }

    private static UiUnavailable Why(UiWorldReadResult w) => new(w.Reason ?? "NoWorld");

    private object Stat(UiBinding b)
    {
        var colon = b.Key!.IndexOf(':');
        var key = new ModValueKey(b.Key[..colon], b.Key[(colon + 1)..]);
        ModValueReadResult r;
        try { r = UiStats.Read(_scope.Mod, key); }
        catch (Exception e) when (e is not OutOfMemoryException) { r = new ModValueReadResult(false, e.Message, null); }
        if (b.Part == "available") return r is { Ok: true, Value: not null };
        if (r is not { Ok: true, Value: { } v }) return new UiUnavailable(r.Reason ?? "NoValue");
        return b.Part switch
        {
            "level" => (ulong)Math.Max(0, v.Level),
            "revision" => v.ValueRevision,
            _ => (decimal)v.Value / Pow10(Math.Max(0, v.Scale)),
        };
    }

    private static decimal Pow10(int scale)
    {
        var p = 1m;
        for (var i = 0; i < scale; i++) p *= 10;
        return p;
    }
}
