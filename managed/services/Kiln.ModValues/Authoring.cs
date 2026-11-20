// SPDX-License-Identifier: MIT
// mod-values/*.json (chapter 05b section 4.1; schema/kiln-mod-values.schema.json, lint KL350-KL355): read for every
// loaded mod at Kiln.ModValues' start and whenever a mod loads again. Both sides describe the values (their names,
// kinds and formats are known before any server answers); the side that runs the world defines them. Strict JSON (no
// comments, no trailing commas), camelCase, lowercase enum names, 64-bit numbers as base-10 strings; an unknown field,
// a duplicate id or a broken rule is an author error that skips the file (logged).
using System.Text.Json;

namespace Kiln;

internal static class ModValuesAuthoring
{
    internal const string Folder = "mod-values";

    private static readonly HashSet<string> TopKeys = new(StringComparer.Ordinal) { "$schema", "format", "values" };

    private static readonly HashSet<string> ValueKeys = new(StringComparer.Ordinal)
    {
        "kind", "label", "numberFormat", "min", "max", "initial", "scale", "thresholds", "persistence", "trustClientImports", "version",
        "carryEpoch", "readPolicy", "readMods", "sync", "allowPartitionMove", "resetOnKindChange",
    };

    // owner -> the mod directory its JSON came from (a mod seen unloaded retires its JSON values)
    private static readonly Dictionary<string, string> Loaded = new(StringComparer.Ordinal);

    /// <summary>Reads one file into definitions of <paramref name="owner"/>. Throws <see cref="ArgumentException"/> with
    /// the file's error.</summary>
    internal static IReadOnlyList<Def> Parse(string owner, string json, string file)
    {
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(json, new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Disallow, AllowTrailingCommas = false, MaxDepth = 16 });
        }
        catch (JsonException e)
        {
            throw new ArgumentException($"{file}: not JSON: {e.Message}");
        }
        using (doc)
        {
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) throw new ArgumentException($"{file}: the file is an object {{\"format\": 1, \"values\": {{...}}}}");
            Unknown(root, TopKeys, file);
            if (!root.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.Number || !format.TryGetInt32(out var f) || f != 1)
                throw new ArgumentException($"{file}: \"format\": 1 is required");
            if (!root.TryGetProperty("values", out var values) || values.ValueKind != JsonValueKind.Object) throw new ArgumentException($"{file}: \"values\" is an object of id -> definition");
            var list = new List<Def>();
            var ids = new HashSet<string>(StringComparer.Ordinal);
            foreach (var p in values.EnumerateObject())
            {
                if (!ids.Add(p.Name)) throw new ArgumentException($"{file}: \"{p.Name}\" is defined twice");
                list.Add(One(owner, p.Name, p.Value, $"{file}: {p.Name}"));
            }
            return list;
        }
    }

    private static void Unknown(JsonElement o, HashSet<string> allowed, string where)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var p in o.EnumerateObject())
        {
            if (!allowed.Contains(p.Name)) throw new ArgumentException($"{where}: unknown field \"{p.Name}\"");
            if (!seen.Add(p.Name)) throw new ArgumentException($"{where}: \"{p.Name}\" is given twice");
        }
    }

    private static Def One(string owner, string id, JsonElement v, string where)
    {
        if (v.ValueKind != JsonValueKind.Object) throw new ArgumentException($"{where}: a definition is an object");
        Unknown(v, ValueKeys, where);
        var kind = Enum<ModValueKind>(v, "kind", null, where, ("counter", ModValueKind.Counter), ("currency", ModValueKind.Currency), ("experience", ModValueKind.Experience));
        if (!v.TryGetProperty("label", out var labelEl)) throw new ArgumentException($"{where}: \"label\" is required");
        var label = Text(labelEl, where + ": label");
        var format = v.TryGetProperty("numberFormat", out var fe) ? Text(fe, where + ": numberFormat") : null;
        var min = Int64(v, "min", where) ?? 0;
        var max = Int64(v, "max", where) ?? long.MaxValue;
        var initial = Int64(v, "initial", where);
        var scale = (int)(UInt(v, "scale", where, 6) ?? 0);
        long[]? thresholds = null;
        if (v.TryGetProperty("thresholds", out var te))
        {
            if (te.ValueKind != JsonValueKind.Array) throw new ArgumentException($"{where}: thresholds is an array of base-10 strings");
            thresholds = te.EnumerateArray().Select((x, i) => x.ValueKind == JsonValueKind.String && ModValuesWire.TryI64(x.GetString(), out var n)
                ? n : throw new ArgumentException($"{where}: thresholds[{i}] is not a base-10 string of a 64-bit integer")).ToArray();
        }
        var persistence = Enum(v, "persistence", ModValuePersistence.World, where, ("world", ModValuePersistence.World), ("carried", ModValuePersistence.Carried));
        var read = Enum(v, "readPolicy", ModValueReadPolicy.OwnerOnly, where, ("ownerOnly", ModValueReadPolicy.OwnerOnly), ("public", ModValueReadPolicy.Public), ("allowList", ModValueReadPolicy.AllowList));
        var sync = Enum(v, "sync", ModValueSyncPolicy.OwnerMod, where, ("none", ModValueSyncPolicy.None), ("ownerMod", ModValueSyncPolicy.OwnerMod), ("readers", ModValueSyncPolicy.Readers));
        string[]? readMods = null;
        if (v.TryGetProperty("readMods", out var re))
        {
            if (re.ValueKind != JsonValueKind.Array) throw new ArgumentException($"{where}: readMods is an array of mod ids");
            readMods = re.EnumerateArray().Select(x => x.ValueKind == JsonValueKind.String ? x.GetString()! : throw new ArgumentException($"{where}: readMods holds strings")).ToArray();
        }
        return Def.Create(owner, id, kind, label, format, min, max, initial, scale, thresholds, persistence, Bool(v, "trustClientImports", where),
            (uint)(UInt(v, "version", where, uint.MaxValue) ?? 1), (uint)(UInt(v, "carryEpoch", where, uint.MaxValue) ?? 1), read, readMods, sync,
            Bool(v, "allowPartitionMove", where), Bool(v, "resetOnKindChange", where));
    }

    private static T Enum<T>(JsonElement v, string name, T? fallback, string where, params (string Name, T Value)[] names) where T : struct
    {
        if (!v.TryGetProperty(name, out var e)) return fallback ?? throw new ArgumentException($"{where}: \"{name}\" is required");
        var s = e.ValueKind == JsonValueKind.String ? e.GetString() : null;
        foreach (var (n, value) in names)
            if (n == s) return value;
        throw new ArgumentException($"{where}: {name} is one of {string.Join(", ", names.Select(x => x.Name))}");
    }

    private static SortedDictionary<string, string> Text(JsonElement e, string where) => ModValueRules.MapOfJson(e.GetRawText(), where);

    private static long? Int64(JsonElement v, string name, string where)
    {
        if (!v.TryGetProperty(name, out var e)) return null;
        return e.ValueKind == JsonValueKind.String && ModValuesWire.TryI64(e.GetString(), out var n)
            ? n : throw new ArgumentException($"{where}: {name} is a base-10 string of a 64-bit integer (\"{e.GetRawText()}\")");
    }

    private static ulong? UInt(JsonElement v, string name, string where, ulong max)
    {
        if (!v.TryGetProperty(name, out var e)) return null;
        return e.ValueKind == JsonValueKind.Number && e.TryGetUInt64(out var n) && n <= max ? n : throw new ArgumentException($"{where}: {name} is a whole number up to {max}");
    }

    private static bool Bool(JsonElement v, string name, string where)
    {
        if (!v.TryGetProperty(name, out var e)) return false;
        return e.ValueKind switch { JsonValueKind.True => true, JsonValueKind.False => false, _ => throw new ArgumentException($"{where}: {name} is true or false") };
    }

    /// <summary>The definitions of one mod's folder, file-name order; a file with an error is skipped (logged). Ids
    /// across the mod's files must differ.</summary>
    internal static IReadOnlyList<Def> ReadMod(string owner, string directory, Action<string> error)
    {
        var dir = Path.Combine(directory, Folder);
        var list = new List<Def>();
        if (!Directory.Exists(dir)) return list;
        var ids = new HashSet<string>(StringComparer.Ordinal);
        foreach (var path in Directory.EnumerateFiles(dir, "*.json").Order(StringComparer.Ordinal))
        {
            var name = $"{owner}/{Folder}/{Path.GetFileName(path)}";
            try
            {
                var defs = Parse(owner, File.ReadAllText(path), name);
                if (defs.FirstOrDefault(d => !ids.Add(d.Id)) is { } dup) throw new ArgumentException($"{name}: \"{dup.Id}\" is defined in another file of the mod too");
                list.AddRange(defs);
            }
            catch (Exception e) when (e is ArgumentException or IOException or UnauthorizedAccessException)
            {
                error(e.Message);
            }
        }
        return list;
    }

    private sealed class Loaded
    {
        public required object? Generation;  // the mod's live ModContext when it was read (a reload brings another)
        public required string Directory;
        public required IReadOnlyList<Def> Defs;
        public readonly HashSet<string> Refused = new(StringComparer.Ordinal);  // logged once per read
    }

    private static readonly Dictionary<string, Loaded> Mods_ = new(StringComparer.Ordinal);

    /// <summary>Loaded mods' JSON in, unloaded mods' JSON values retired. At the start, at every world load and every 25
    /// service steps. Each pass also makes sure every value of a loaded mod is described here and, where this process
    /// runs the world (a dedicated server, a client that hosts), active: a client that starts hosting after the main menu
    /// defines what it only described there, and a mod reloaded between passes (a new load generation) is read again.</summary>
    internal static void Poll()
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var server = ModValuesCaps.ServerRole;
        foreach (var m in Mods.All)
        {
            if (!m.IsLoaded || m.Directory.Length == 0) continue;
            var owner = m.Id.ToLowerInvariant();
            if (!ModValueRules.IsOwner(owner)) continue;
            if (!Directory.Exists(Path.Combine(m.Directory, Folder))) continue;
            seen.Add(owner);
            var generation = m.Info is { } info ? ModContext.FindLive(info) : null;
            if (!Mods_.TryGetValue(owner, out var l) || !ReferenceEquals(l.Generation, generation) || l.Directory != m.Directory)
            {
                if (l is not null) ModValuesService.Retire(owner, l.Generation);
                var defs = ReadMod(owner, m.Directory, e => ModValuesService.Log(LogLevel.Error, $"mod-values: {e}"));
                Mods_[owner] = l = new Loaded { Generation = generation, Directory = m.Directory, Defs = defs };
                if (defs.Count > 0) ModValuesService.Log(LogLevel.Info, $"mod-values: {owner}: {defs.Count} value(s) read from {Folder}/*.json");
            }
            Ensure(owner, l, server);
        }
        foreach (var owner in Mods_.Keys.Where(o => !seen.Contains(o)).ToArray())
        {
            ModValuesService.Retire(owner, Mods_[owner].Generation);
            Mods_.Remove(owner);
        }
    }

    /// <summary>Describes what is not described, and defines what is not active when <paramref name="server"/>.</summary>
    private static void Ensure(string owner, Loaded l, bool server)
    {
        foreach (var d in l.Defs)
        {
            var key = d.Key;
            var reg = ModValuesService.Registry;
            if (server ? reg.Active(key) is not null : reg.Described(key) is not null) continue;
            var r = server ? ModValuesService.Define(d, l.Generation) : ModValuesService.Describe(d, l.Generation);
            if (!r.Ok && l.Refused.Add(d.Id)) ModValuesService.Log(LogLevel.Error, $"mod-values: {owner}/{Folder}: {d.Id}: {r.Reason}");
        }
    }

    /// <summary>Tests: what Poll does for one mod, without Mods.All.</summary>
    internal static void EnsureForTests(string owner, IReadOnlyList<Def> defs, object? generation, bool server)
    {
        if (!Mods_.TryGetValue(owner, out var l) || !ReferenceEquals(l.Generation, generation))
        {
            if (l is not null) ModValuesService.Retire(owner, l.Generation);
            Mods_[owner] = l = new Loaded { Generation = generation, Directory = "", Defs = defs };
        }
        Ensure(owner, l, server);
    }
}
