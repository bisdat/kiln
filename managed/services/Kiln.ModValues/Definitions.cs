// SPDX-License-Identifier: MIT
// Definitions of mod values (chapter 05b section 4.1): the author's options checked and normalized once, the
// fingerprint of what a saved value depends on, and the registry of what each owner described and activated in this
// load generation of it. Describe is any thread (a lock); activation and retirement run on the tick thread.
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Kiln;

/// <summary>The syntax and limits of chapter 05b section 4.1.</summary>
internal static partial class ModValueRules
{
    internal const int MaxDefinitionsPerOwner = 128, MaxThresholds = 256, MaxReadMods = 32, MaxScale = 6;
    internal const int MaxLabelBytes = 1024, MaxFormatBytes = 256, MaxDefinitionBytes = 4096;

    [GeneratedRegex("^[a-z][a-z0-9._-]{0,63}$", RegexOptions.CultureInvariant)]
    private static partial Regex IdPattern();

    // A manifest's canonical id (Boot.cs IdPattern), or a native owner's "native." + 32 hex digits (also fits it).
    [GeneratedRegex("^[a-z0-9][a-z0-9._-]{0,63}$", RegexOptions.CultureInvariant)]
    private static partial Regex OwnerPattern();

    internal static bool IsId(string? s) => s is not null && IdPattern().IsMatch(s);

    internal static bool IsOwner(string? s) => s is not null && OwnerPattern().IsMatch(s);

    /// <summary>The namespace of a managed mod: its manifest id in its canonical lowercase spelling.</summary>
    internal static string OwnerOf(ModContext mod) => mod.Info.Id.ToLowerInvariant();

    /// <summary>N0-N6, F0-F6, "0", "0." with 1 to 6 zeros.</summary>
    internal static bool IsNumberFormat(string? f) =>
        f is { Length: 2 } && (f[0] is 'N' or 'F') && f[1] is >= '0' and <= '6'
        || f == "0"
        || f is { Length: >= 3 and <= 8 } && f.StartsWith("0.", StringComparison.Ordinal) && f.AsSpan(2).TrimStart('0').IsEmpty;

    internal static SortedDictionary<string, string> MapOf(UiText text)
    {
        var map = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (var lang in text.Languages) map[lang.ToLowerInvariant()] = text.For(lang);
        return map;
    }

    internal static UiText TextOf(IReadOnlyDictionary<string, string> map) => new(map);

    /// <summary>A UiText as JSON (a language map), as the native facade takes it: a JSON string is one text for all.</summary>
    internal static SortedDictionary<string, string> MapOfJson(string json, string what)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var map = new SortedDictionary<string, string>(StringComparer.Ordinal);
            if (doc.RootElement.ValueKind == JsonValueKind.String) map["*"] = doc.RootElement.GetString()!;
            else if (doc.RootElement.ValueKind == JsonValueKind.Object)
                foreach (var p in doc.RootElement.EnumerateObject())
                {
                    if (p.Value.ValueKind != JsonValueKind.String) throw new ArgumentException($"{what}: \"{p.Name}\" is not a string");
                    map[p.Name.Trim().Replace('_', '-').ToLowerInvariant()] = p.Value.GetString()!;
                }
            else throw new ArgumentException($"{what} must be a string or an object of language -> text");
            if (map.Count == 0) throw new ArgumentException($"{what} is empty");
            return map;
        }
        catch (JsonException e)
        {
            throw new ArgumentException($"{what} is not JSON: {e.Message}");
        }
    }

    internal static int Bytes(SortedDictionary<string, string> map) => Encoding.UTF8.GetByteCount(JsonSerializer.Serialize(map));
}

/// <summary>A checked, normalized definition. Immutable.</summary>
internal sealed class Def
{
    public required string Owner { get; init; }
    public required string Id { get; init; }
    public required ModValueKind Kind { get; init; }
    public required long Min { get; init; }
    public required long Max { get; init; }
    public required long Initial { get; init; }
    public required bool HasInitial { get; init; }
    public required int Scale { get; init; }
    public required long[] Thresholds { get; init; }
    public required ModValuePersistence Persistence { get; init; }
    public required bool TrustImports { get; init; }
    public required uint Version { get; init; }
    public required uint CarryEpoch { get; init; }
    public required ModValueReadPolicy ReadPolicy { get; init; }
    public required string[] ReadMods { get; init; }
    public required ModValueSyncPolicy Sync { get; init; }
    public required bool AllowPartitionMove { get; init; }
    public required bool ResetOnKindChange { get; init; }
    public required SortedDictionary<string, string> Label { get; init; }
    public required SortedDictionary<string, string> NumberFormat { get; init; }

    public ModValueKey Key => new(Owner, Id);
    public string Qualified => Owner + ":" + Id;

    /// <summary>What a saved value depends on: a change needs a higher version (section 4.3). Labels, formats and the
    /// read and sync policies are display-only and not in it; the carry epoch has its own rule.</summary>
    public string Fingerprint { get; private set; } = "";

    /// <summary>Every field, for "the same definition again" (idempotent) against "another definition" (a conflict).</summary>
    public string Canonical { get; private set; } = "";

    /// <summary>The definition as the stream and the native facade publish it.</summary>
    public string PublicJson { get; private set; } = "";

    public int LevelOf(long raw)
    {
        if (Kind != ModValueKind.Experience) return 0;
        // thresholds start at 0 and increase: the level is how many of them raw has reached
        int lo = 0, hi = Thresholds.Length;
        while (lo < hi)
        {
            var mid = (lo + hi) / 2;
            if (Thresholds[mid] <= raw) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    public bool Readable(string? reader) =>
        reader == Owner || ReadPolicy == ModValueReadPolicy.Public || ReadPolicy == ModValueReadPolicy.AllowList && reader is not null && Array.IndexOf(ReadMods, reader) >= 0;

    public ModValueDefinition ToPublic(bool active) =>
        new(Key, Kind, ModValueRules.TextOf(Label), ModValueRules.TextOf(NumberFormat), Min, Max, Initial, Scale, Thresholds.ToArray(),
            Persistence, Version, CarryEpoch, ReadPolicy, ReadMods.ToArray(), Sync, active);

    public static Def FromOptions(string owner, string id, ModValueOptions o)
    {
        ArgumentNullException.ThrowIfNull(o);
        if (o.Label is null) throw new ArgumentException("a mod value needs a label", nameof(o));
        return Create(owner, id, o.Kind, ModValueRules.MapOf(o.Label), o.NumberFormat is null ? null : ModValueRules.MapOf(o.NumberFormat),
            o.Min, o.Max, o.Initial, o.Scale, o.Thresholds, o.Persistence, o.TrustClientImports, o.Version, o.CarryEpoch, o.ReadPolicy,
            o.ReadMods, o.Sync, o.AllowPartitionMove, o.ResetOnKindChange);
    }

    /// <summary>Checks and normalizes; an author error throws <see cref="ArgumentException"/> naming the value.</summary>
    public static Def Create(string owner, string id, ModValueKind kind, SortedDictionary<string, string> label,
        SortedDictionary<string, string>? numberFormat, long min, long max, long? initial, int scale, IReadOnlyList<long>? thresholds,
        ModValuePersistence persistence, bool trust, uint version, uint carryEpoch, ModValueReadPolicy readPolicy,
        IReadOnlyList<string>? readMods, ModValueSyncPolicy sync, bool allowMove, bool resetOnKind)
    {
        if (!ModValueRules.IsOwner(owner)) throw new ArgumentException($"\"{owner}\" is not a mod id in its canonical lowercase spelling");
        var name = $"mod value {owner}:{id}";
        if (!ModValueRules.IsId(id)) throw new ArgumentException($"\"{id}\" is not a mod value id: [a-z][a-z0-9._-], at most 64 characters", nameof(id));
        if (!Enum.IsDefined(kind)) throw new ArgumentException($"{name}: unknown kind {(int)kind}");
        if (!Enum.IsDefined(persistence)) throw new ArgumentException($"{name}: unknown persistence {(int)persistence}");
        if (!Enum.IsDefined(readPolicy)) throw new ArgumentException($"{name}: unknown read policy {(int)readPolicy}");
        if (!Enum.IsDefined(sync)) throw new ArgumentException($"{name}: unknown sync policy {(int)sync}");
        if (scale is < 0 or > ModValueRules.MaxScale) throw new ArgumentException($"{name}: scale is 0 to 6, not {scale}");
        if (min > max) throw new ArgumentException($"{name}: min {min} is above max {max}");
        if (kind is ModValueKind.Currency or ModValueKind.Experience && min < 0) throw new ArgumentException($"{name}: a {Lower(kind)} starts at 0 or more (min {min})");
        if (initial is { } i && (i < min || i > max)) throw new ArgumentException($"{name}: initial {i} is outside {min}..{max}");
        if (version == 0) throw new ArgumentException($"{name}: version starts at 1");
        if (carryEpoch == 0) throw new ArgumentException($"{name}: carryEpoch starts at 1");
        long[] t = thresholds?.ToArray() ?? [];
        if (kind == ModValueKind.Experience)
        {
            if (scale != 0) throw new ArgumentException($"{name}: experience has scale 0");
            if (t.Length == 0) throw new ArgumentException($"{name}: experience needs thresholds (the first is 0)");
            if (t.Length > ModValueRules.MaxThresholds) throw new ArgumentException($"{name}: at most {ModValueRules.MaxThresholds} thresholds");
            if (t[0] != 0) throw new ArgumentException($"{name}: the first threshold is 0");
            for (var k = 1; k < t.Length; k++)
                if (t[k] <= t[k - 1]) throw new ArgumentException($"{name}: thresholds increase strictly ({t[k - 1]} then {t[k]})");
        }
        else if (t.Length > 0) throw new ArgumentException($"{name}: only experience has thresholds");
        if (persistence == ModValuePersistence.Carried)
        {
            if (kind == ModValueKind.Currency) throw new ArgumentException($"{name}: a currency never travels between worlds (persistence world)");
            if (!trust) throw new ArgumentException($"{name}: carried values come from the player's copy: set trustClientImports");
            if (kind == ModValueKind.Counter && min < 0) throw new ArgumentException($"{name}: a carried counter is nonnegative progress (min 0 or more)");
        }
        var mods = (readMods ?? []).ToArray();
        if (mods.Length > 0 && readPolicy != ModValueReadPolicy.AllowList) throw new ArgumentException($"{name}: readMods go with readPolicy allowList");
        if (mods.Length > ModValueRules.MaxReadMods) throw new ArgumentException($"{name}: at most {ModValueRules.MaxReadMods} readMods");
        foreach (var m in mods)
            if (!ModValueRules.IsOwner(m)) throw new ArgumentException($"{name}: readMods: \"{m}\" is not a mod id");
        mods = mods.Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
        if (label.Count == 0) throw new ArgumentException($"{name}: the label is empty");
        if (ModValueRules.Bytes(label) > ModValueRules.MaxLabelBytes) throw new ArgumentException($"{name}: the label's languages take more than {ModValueRules.MaxLabelBytes} bytes");
        var format = numberFormat is { Count: > 0 } ? numberFormat : new SortedDictionary<string, string>(StringComparer.Ordinal) { ["*"] = "N" + scale };
        foreach (var (lang, f) in format)
            if (!ModValueRules.IsNumberFormat(f)) throw new ArgumentException($"{name}: number format \"{f}\" ({lang}) is not one of N0-N6, F0-F6, 0, 0.0 to 0.000000");
        if (ModValueRules.Bytes(format) > ModValueRules.MaxFormatBytes) throw new ArgumentException($"{name}: the number formats take more than {ModValueRules.MaxFormatBytes} bytes");
        var d = new Def
        {
            Owner = owner, Id = id, Kind = kind, Min = min, Max = max, Initial = initial ?? min, HasInitial = initial is not null,
            Scale = scale, Thresholds = t, Persistence = persistence, TrustImports = trust, Version = version, CarryEpoch = carryEpoch,
            ReadPolicy = readPolicy, ReadMods = mods, Sync = sync, AllowPartitionMove = allowMove, ResetOnKindChange = resetOnKind,
            Label = new SortedDictionary<string, string>(label, StringComparer.Ordinal), NumberFormat = new SortedDictionary<string, string>(format, StringComparer.Ordinal),
        };
        d.Seal();
        if (Encoding.UTF8.GetByteCount(d.PublicJson) > ModValueRules.MaxDefinitionBytes)
            throw new ArgumentException($"{name}: the definition takes more than {ModValueRules.MaxDefinitionBytes} bytes");
        return d;
    }

    internal static string Lower(ModValueKind k) => k switch { ModValueKind.Counter => "counter", ModValueKind.Currency => "currency", _ => "experience" };

    private void Seal()
    {
        static string S(long v) => v.ToString(System.Globalization.CultureInfo.InvariantCulture);
        var semantic = $"kind={(int)Kind};min={S(Min)};max={S(Max)};initial={(HasInitial ? S(Initial) : "min")};scale={Scale};" +
                       $"thresholds={string.Join(',', Thresholds.Select(S))};persistence={(int)Persistence}";
        Fingerprint = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(semantic)))[..32];
        Canonical = semantic + $";trust={TrustImports};version={Version};epoch={CarryEpoch};read={(int)ReadPolicy};readMods={string.Join(',', ReadMods)};" +
                    $"sync={(int)Sync};move={AllowPartitionMove};resetKind={ResetOnKindChange};label={JsonSerializer.Serialize(Label)};format={JsonSerializer.Serialize(NumberFormat)}";
        PublicJson = JsonSerializer.Serialize(Wire(), ModValuesWire.Options);
    }

    /// <summary>The definition as the stream sends it.</summary>
    public ModValuesWire.Definition Wire() => new()
    {
        Key = Qualified, Version = Version, Kind = (uint)Kind, Persistence = (uint)Persistence, Scale = (uint)Scale, CarryEpoch = CarryEpoch,
        Label = new SortedDictionary<string, string>(Label, StringComparer.Ordinal), NumberFormat = new SortedDictionary<string, string>(NumberFormat, StringComparer.Ordinal),
        ReadPolicy = (uint)ReadPolicy, ReadMods = ReadMods.ToArray(), Sync = (uint)Sync,
    };
}

/// <summary>What each owner described and activated. Every definition remembers who registered it (its token: the
/// ModContext of a managed mod's load generation, a native owner's handle, null for Kiln's own reads of a mod's JSON
/// files); a retirement takes only that token's definitions out of the active view, and a token whose mod context is no
/// longer alive counts as retired. Saved values are parked, never deleted, and come back with the next definition.</summary>
internal sealed class Registry
{
    private readonly object _gate = new();
    private readonly Dictionary<string, Owner> _owners = new(StringComparer.Ordinal);
    private int _version;

    private sealed class Entry(Def def, object? token)
    {
        public Def Def = def;
        public object? Token = token;
    }

    private sealed class Owner
    {
        public readonly Dictionary<string, Entry> Described = new(StringComparer.Ordinal);
        public readonly Dictionary<string, Entry> Active = new(StringComparer.Ordinal);
        public readonly HashSet<string> Retired = new(StringComparer.Ordinal);
    }

    /// <summary>Changes whenever a definition is activated, changed or retired (the stream sends a new full).</summary>
    public int Version => Volatile.Read(ref _version);

    /// <summary>Where refusals that name two owners are reported (Kiln's log in the service).</summary>
    public Action<string>? Report { get; set; }

    private static bool Stale(object? token) => token is ModContext m && !m.Alive;

    /// <summary>Retires what tokens of an earlier load generation registered (their mod unloaded, a reload).</summary>
    private void Prune(Owner o)
    {
        foreach (var id in o.Described.Where(kv => Stale(kv.Value.Token)).Select(kv => kv.Key).ToList()) o.Described.Remove(id);
        foreach (var id in o.Active.Where(kv => Stale(kv.Value.Token)).Select(kv => kv.Key).ToList())
        {
            o.Active.Remove(id);
            o.Retired.Add(id);
            _version++;
        }
    }

    private Owner OwnerOf(string owner)
    {
        if (!_owners.TryGetValue(owner, out var o)) _owners[owner] = o = new Owner();
        Prune(o);
        return o;
    }

    /// <summary>Registers the metadata. Null on success, else a reason.</summary>
    public string? Describe(Def d, object? token, out bool added)
    {
        added = false;
        lock (_gate)
        {
            var o = OwnerOf(d.Owner);
            if (o.Described.TryGetValue(d.Id, out var have))
            {
                if (have.Def.Canonical != d.Canonical) return ModValueReasons.DefinitionConflict;
                have.Token ??= token;  // the mod's own code took over what Kiln read from its files
                return null;
            }
            if (o.Described.Count >= ModValueRules.MaxDefinitionsPerOwner) return ModValueReasons.Budget;
            o.Described[d.Id] = new Entry(d, token);
            o.Retired.Remove(d.Id);
            added = true;
            return null;
        }
    }

    /// <summary>Describes (idempotently) and activates. Null on success, else a reason.</summary>
    public string? Activate(Def d, object? token, out bool activated)
    {
        activated = false;
        lock (_gate)
        {
            var why = Describe(d, token, out _);
            if (why is not null) return why;
            var o = _owners[d.Owner];
            if (o.Active.TryGetValue(d.Id, out var have))
            {
                if (have.Def.Canonical != d.Canonical) return ModValueReasons.DefinitionConflict;
                have.Token ??= token;
                return null;
            }
            o.Active[d.Id] = new Entry(d, token);
            activated = true;
            _version++;
            return null;
        }
    }

    /// <summary>Takes the definitions <paramref name="token"/> registered (null: all of the owner's) out of the
    /// described and active views; their data stays. Returns what was active.</summary>
    public IReadOnlyList<Def> Retire(string owner, object? token = null)
    {
        lock (_gate)
        {
            if (!_owners.TryGetValue(owner, out var o)) return [];
            bool Mine(Entry e) => token is null || Equals(e.Token, token);
            var was = o.Active.Where(kv => Mine(kv.Value)).Select(kv => kv.Value.Def).ToList();
            foreach (var d in was)
            {
                o.Active.Remove(d.Id);
                o.Retired.Add(d.Id);
            }
            foreach (var id in o.Described.Where(kv => Mine(kv.Value)).Select(kv => kv.Key).ToList()) o.Described.Remove(id);
            if (was.Count > 0) _version++;
            return was;
        }
    }

    public Def? Active(ModValueKey key)
    {
        lock (_gate)
        {
            if (!_owners.TryGetValue(key.Mod, out var o)) return null;
            Prune(o);
            return o.Active.GetValueOrDefault(key.Id)?.Def;
        }
    }

    public Def? Described(ModValueKey key)
    {
        lock (_gate)
        {
            if (!_owners.TryGetValue(key.Mod, out var o)) return null;
            Prune(o);
            return o.Described.GetValueOrDefault(key.Id)?.Def;
        }
    }

    public bool IsRetired(ModValueKey key)
    {
        lock (_gate)
        {
            if (!_owners.TryGetValue(key.Mod, out var o)) return false;
            Prune(o);
            return o.Retired.Contains(key.Id);
        }
    }

    public IReadOnlyList<Def> AllActive()
    {
        lock (_gate)
        {
            foreach (var o in _owners.Values) Prune(o);
            return _owners.Values.SelectMany(o => o.Active.Values).Select(e => e.Def).OrderBy(d => d.Qualified, StringComparer.Ordinal).ToList();
        }
    }

    public IReadOnlyList<Def> AllDescribed()
    {
        lock (_gate)
        {
            foreach (var o in _owners.Values) Prune(o);
            return _owners.Values.SelectMany(o => o.Described.Values).Select(e => e.Def).OrderBy(d => d.Qualified, StringComparer.Ordinal).ToList();
        }
    }

    public void Clear()
    {
        lock (_gate)
        {
            _owners.Clear();
            _version++;
        }
    }
}
