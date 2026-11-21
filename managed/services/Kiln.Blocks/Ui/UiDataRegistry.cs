// SPDX-License-Identifier: MIT
// A mod's own data for its UI blocks (dossier 09 §4.2): BlockUi.Bindings.Define declares a source (a scalar, rows or a
// tree, with typed fields) before the blocks that bind it as mod.<id> compile; Publish copies a snapshot at once (checked
// whole: a bad tree keeps the previous one) and queues it; UiRuntime.Pump commits it, advances the source's version and
// posts UiBindingValueChangedEvent. A renderer only ever reads the last committed immutable value: no publisher callback
// runs from a frame. Own-mod only: mod.<id> names the binding mod's sources.
using System.Collections.Immutable;
using System.Text;
using System.Text.RegularExpressions;

namespace Kiln;

internal static partial class UiDataRegistry
{
    internal const int MaxSourcesPerMod = 64, MaxFields = 16, MaxEntries = 512, MaxDepth = 8;
    internal const int MaxPublicationBytes = 64 * 1024, MaxModBytes = 256 * 1024;

    private static readonly HashSet<string> RowBuiltIns = new(StringComparer.Ordinal) { "key" };
    private static readonly HashSet<string> NodeBuiltIns = new(StringComparer.Ordinal) { "key", "parent", "label", "depth" };

    [GeneratedRegex("^[A-Za-z0-9._:-]{1,128}$", RegexOptions.CultureInvariant)]
    internal static partial Regex KeyPattern();

    private sealed class Source(string mod, string id, UiDataSchema schema)
    {
        public readonly string Mod = mod, Id = id;
        public readonly UiDataSchema Schema = schema;
        public object? Latest;
        public int LatestBytes;
        public ulong Version;
        public (object Value, int Bytes, TaskCompletionSource<UiDataPublishResult> Done)? Pending;
        public bool Removed;
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<(string Mod, string Id), Source> Sources = new();

    public static UiDataSchema? SchemaOf(string mod, string id)
    {
        lock (Gate) return Sources.TryGetValue((mod, id), out var s) ? s.Schema : null;
    }

    /// <summary>The last committed value: a scalar box, a <see cref="UiRowsValue"/> or a <see cref="UiTreeValue"/>; null
    /// before the first publication.</summary>
    public static object? Latest(string mod, string id)
    {
        lock (Gate) return Sources.TryGetValue((mod, id), out var s) ? s.Latest : null;
    }

    public static ulong VersionOf(string mod, string id)
    {
        lock (Gate) return Sources.TryGetValue((mod, id), out var s) ? s.Version : 0;
    }

    public static UiDataRegistration Define(string mod, string id, UiDataSchema schema)
    {
        ArgumentNullException.ThrowIfNull(schema);
        if (!UiTypedNames.IsId(id)) throw new ArgumentException($"\"{id}\" is not a data source id: [a-z][a-z0-9_-]{{0,63}}", nameof(id));
        CheckSchema(schema);
        lock (Gate)
        {
            if (Sources.ContainsKey((mod, id))) return new UiDataRegistration(false, $"AlreadyDefined: mod {mod} already defined data source \"{id}\"", id, null);
            if (Sources.Keys.Count(k => k.Mod == mod) >= MaxSourcesPerMod)
                return new UiDataRegistration(false, $"{UiStateReasons.Budget}: at most {MaxSourcesPerMod} data sources per mod", id, null);
            var source = new Source(mod, id, schema);
            Sources[(mod, id)] = source;
            return new UiDataRegistration(true, null, id, () => Remove(source));
        }
    }

    private static void Remove(Source s)
    {
        TaskCompletionSource<UiDataPublishResult>? pending;
        lock (Gate)
        {
            if (Sources.TryGetValue((s.Mod, s.Id), out var cur) && ReferenceEquals(cur, s)) Sources.Remove((s.Mod, s.Id));
            s.Removed = true;
            pending = s.Pending?.Done;
            s.Pending = null;
        }
        pending?.TrySetResult(new UiDataPublishResult(false, $"{UiStateReasons.Unloaded}: the data source was removed", 0));
    }

    private static void CheckSchema(UiDataSchema schema)
    {
        if (!Enum.IsDefined(schema.Shape)) throw new ArgumentException($"{schema.Shape} is not a data shape", nameof(schema));
        if (schema.Shape == UiDataShape.Scalar)
        {
            if (schema.ScalarType is not { } t || !Enum.IsDefined(t)) throw new ArgumentException("a scalar source needs its ScalarType", nameof(schema));
            if (schema.Fields is { Count: > 0 }) throw new ArgumentException("a scalar source has no fields", nameof(schema));
            return;
        }
        if (schema.ScalarType != null) throw new ArgumentException("rows and trees have fields, not a ScalarType", nameof(schema));
        CheckFields(schema.Shape, schema.Fields ?? []);
    }

    /// <summary>Fields of rows or tree nodes: at most 16, ids unique, none of the built-in names.</summary>
    internal static void CheckFields(UiDataShape shape, IReadOnlyList<UiField> fields)
    {
        if (fields.Count > MaxFields) throw new ArgumentException($"at most {MaxFields} fields", nameof(fields));
        var reserved = shape == UiDataShape.Rows ? RowBuiltIns : NodeBuiltIns;
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var f in fields)
        {
            if (f == null || !UiTypedNames.IsId(f.Id)) throw new ArgumentException($"\"{f?.Id}\" is not a field id: [a-z][a-z0-9_-]{{0,63}}", nameof(fields));
            if (reserved.Contains(f.Id)) throw new ArgumentException($"\"{f.Id}\" is a built-in field of every {(shape == UiDataShape.Rows ? "row" : "node")}", nameof(fields));
            if (!Enum.IsDefined(f.Type)) throw new ArgumentException($"field {f.Id}: {f.Type} is not a type", nameof(fields));
            if (!seen.Add(f.Id)) throw new ArgumentException($"field \"{f.Id}\" twice", nameof(fields));
        }
    }

    /// <summary>Copies and checks a publication, then queues it for the next Pump. A replaced queued publication answers Superseded.</summary>
    public static Task<UiDataPublishResult> Publish(string mod, string id, UiDataSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        Source? s;
        lock (Gate) Sources.TryGetValue((mod, id), out s);
        if (s == null) return Task.FromResult(new UiDataPublishResult(false, $"{UiStateReasons.UnknownKey}: mod {mod} has no data source \"{id}\"", 0));
        object value;
        int bytes;
        try { (value, bytes) = Build(s.Schema, snapshot); }
        catch (UiDataRefusal r) { return Task.FromResult(new UiDataPublishResult(false, r.Message, s.Version)); }
        if (bytes > MaxPublicationBytes) return Task.FromResult(new UiDataPublishResult(false, $"{UiStateReasons.Budget}: {bytes} bytes is more than {MaxPublicationBytes} per publication", s.Version));
        var done = new TaskCompletionSource<UiDataPublishResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        TaskCompletionSource<UiDataPublishResult>? replaced;
        lock (Gate)
        {
            if (s.Removed) return Task.FromResult(new UiDataPublishResult(false, $"{UiStateReasons.Unloaded}: the data source was removed", 0));
            var others = Sources.Values.Where(x => x.Mod == mod && !ReferenceEquals(x, s)).Sum(x => x.LatestBytes);
            if (others + bytes > MaxModBytes)
                return Task.FromResult(new UiDataPublishResult(false, $"{UiStateReasons.Budget}: the mod's data would be {others + bytes} bytes, more than {MaxModBytes}", s.Version));
            replaced = s.Pending?.Done;
            s.Pending = (value, bytes, done);
        }
        replaced?.TrySetResult(new UiDataPublishResult(false, $"{UiStateReasons.Superseded}: a newer publication replaced it before it was committed", 0));
        UiRuntime.WakeForData();
        return done.Task;
    }

    /// <summary>Commits the queued publications. Pump.</summary>
    public static void Commit()
    {
        List<(Source S, ulong Old, ulong New, TaskCompletionSource<UiDataPublishResult> Done)>? committed = null;
        lock (Gate)
            foreach (var s in Sources.Values)
                if (s.Pending is { } p)
                {
                    s.Pending = null;
                    var old = s.Version;
                    s.Version = old + 1;
                    s.Latest = p.Value switch
                    {
                        UiRowsValue r => r with { Version = s.Version },
                        UiTreeValue t => t with { Version = s.Version },
                        var x => x,
                    };
                    s.LatestBytes = p.Bytes;
                    (committed ??= new()).Add((s, old, s.Version, p.Done));
                }
        if (committed == null) return;
        foreach (var (s, old, now, done) in committed)
        {
            done.TrySetResult(new UiDataPublishResult(true, null, now));
            if (Events.HasSubscribers<UiBindingValueChangedEvent>()) Events.Post(new UiBindingValueChangedEvent(s.Mod, s.Id, old, now));
        }
    }

    public static void Forget(string mod)
    {
        Source[] gone;
        lock (Gate) gone = [.. Sources.Values.Where(s => s.Mod == mod)];
        foreach (var s in gone) Remove(s);
    }

    internal static void ResetForTests()
    {
        Source[] all;
        lock (Gate) all = [.. Sources.Values];
        foreach (var s in all) Remove(s);
    }

    // ---- building immutable values -------------------------------------------------------------------------------

    internal sealed class UiDataRefusal(string message) : Exception(message);

    private static (object Value, int Bytes) Build(UiDataSchema schema, UiDataSnapshot snapshot)
    {
        switch (schema.Shape, snapshot)
        {
            case (UiDataShape.Scalar, UiDataSnapshot.Scalar s):
                if (s.Value == null || s.Value.Type != schema.ScalarType) throw new ArgumentException($"the source holds a {schema.ScalarType}, not a {s.Value?.Type}");
                Check(s.Value, "the value");
                return (UiTypedNames.Box(s.Value), Size(s.Value));
            case (UiDataShape.Rows, UiDataSnapshot.Rows r):
                return Rows(schema.Fields ?? [], r.Values ?? throw new ArgumentException("rows: Values is null"));
            case (UiDataShape.Tree, UiDataSnapshot.Tree t):
                var nodes = t.Values ?? throw new ArgumentException("tree: Values is null");
                return Tree(schema.Fields ?? [], nodes.Select(n => n == null ? throw new ArgumentException("a null node") : (n.Key, (string?)n.ParentKey, n.Label, (IReadOnlyDictionary<string, UiScalar>?)n.Values)).ToList());
            default:
                throw new ArgumentException($"the source is a {schema.Shape} source; a {snapshot.GetType().Name} publication does not fit it");
        }
    }

    private static (object, int) Rows(IReadOnlyList<UiField> fields, IReadOnlyList<UiDataRow> rows)
    {
        if (rows.Count > MaxEntries) throw new UiDataRefusal($"{UiStateReasons.Budget}: {rows.Count} rows is more than {MaxEntries}");
        var keys = new HashSet<string>(StringComparer.Ordinal);
        var b = ImmutableArray.CreateBuilder<UiRowEntry>(rows.Count);
        var bytes = 0;
        foreach (var row in rows)
        {
            if (row == null) throw new ArgumentException("a null row");
            if (row.Key == null || !KeyPattern().IsMatch(row.Key)) throw new UiDataRefusal($"InvalidData: row key \"{row.Key}\" is not [A-Za-z0-9._:-]{{1,128}}");
            if (!keys.Add(row.Key)) throw new UiDataRefusal($"InvalidData: row key \"{row.Key}\" twice");
            var (values, n) = Values(fields, row.Values, $"row {row.Key}");
            bytes += n + row.Key.Length;
            b.Add(new UiRowEntry(row.Key, values.Add("key", row.Key)));
        }
        return (new UiRowsValue(b.MoveToImmutable(), 0), bytes);
    }

    /// <summary>Checks and orders a tree (each node before its children, children in the given order); refuses it whole when a
    /// key repeats, a parent is missing, the parents loop, it is deeper than 8 or has more than 512 nodes.</summary>
    internal static (UiTreeValue, int) Tree(IReadOnlyList<UiField> fields, IReadOnlyList<(string Key, string? Parent, UiText Label, IReadOnlyDictionary<string, UiScalar>? Values)> nodes)
    {
        if (nodes.Count > MaxEntries) throw new UiDataRefusal($"{UiStateReasons.Budget}: {nodes.Count} nodes is more than {MaxEntries}");
        var byKey = new Dictionary<string, int>(StringComparer.Ordinal);
        for (var i = 0; i < nodes.Count; i++)
        {
            var k = nodes[i].Key;
            if (k == null || !KeyPattern().IsMatch(k)) throw new UiDataRefusal($"InvalidData: node key \"{k}\" is not [A-Za-z0-9._:-]{{1,128}}");
            if (!byKey.TryAdd(k, i)) throw new UiDataRefusal($"InvalidData: node key \"{k}\" twice in the tree");
            if (nodes[i].Label == null) throw new ArgumentException($"node {k} has no label");
        }
        var children = new Dictionary<string, List<int>>(StringComparer.Ordinal);
        var roots = new List<int>();
        for (var i = 0; i < nodes.Count; i++)
        {
            var p = nodes[i].Parent;
            if (p == null) { roots.Add(i); continue; }
            if (!byKey.ContainsKey(p)) throw new UiDataRefusal($"InvalidData: node \"{nodes[i].Key}\" names a parent \"{p}\" that is not in the tree");
            if (!children.TryGetValue(p, out var list)) children[p] = list = new();
            list.Add(i);
        }
        var order = ImmutableArray.CreateBuilder<UiTreeEntry>(nodes.Count);
        var bytes = 0;
        void Visit(int i, int depth)
        {
            if (depth >= MaxDepth) throw new UiDataRefusal($"InvalidData: the tree is deeper than {MaxDepth} levels at \"{nodes[i].Key}\"");
            var n = nodes[i];
            var (values, size) = Values(fields, n.Values, $"node {n.Key}");
            bytes += size + n.Key.Length + n.Label.Languages.Sum(l => Encoding.UTF8.GetByteCount(n.Label.For(l)));
            var kids = children.TryGetValue(n.Key, out var c) ? c : null;
            order.Add(new UiTreeEntry(n.Key, n.Parent, n.Label, depth, kids is { Count: > 0 }, values));
            if (kids != null) foreach (var k in kids) Visit(k, depth + 1);
        }
        foreach (var r in roots) Visit(r, 0);
        // a node not reached from a root sits on a loop of parents
        if (order.Count != nodes.Count)
        {
            var reached = order.Select(e => e.Key).ToHashSet(StringComparer.Ordinal);
            throw new UiDataRefusal($"InvalidData: the parents of \"{nodes.First(n => !reached.Contains(n.Key)).Key}\" make a loop");
        }
        return (new UiTreeValue(order.MoveToImmutable(), 0), bytes);
    }

    private static (ImmutableDictionary<string, object>, int) Values(IReadOnlyList<UiField> fields, IReadOnlyDictionary<string, UiScalar>? given, string where)
    {
        given ??= ImmutableDictionary<string, UiScalar>.Empty;
        var b = ImmutableDictionary.CreateBuilder<string, object>(StringComparer.Ordinal);
        var bytes = 0;
        foreach (var f in fields)
        {
            if (!given.TryGetValue(f.Id, out var v) || v == null) throw new ArgumentException($"{where}: field \"{f.Id}\" is missing");
            if (v.Type != f.Type) throw new ArgumentException($"{where}: field \"{f.Id}\" is a {f.Type}, not a {v.Type}");
            Check(v, $"{where}: field {f.Id}");
            b[f.Id] = UiTypedNames.Box(v);
            bytes += Size(v) + f.Id.Length;
        }
        foreach (var k in given.Keys)
            if (!fields.Any(f => f.Id == k)) throw new ArgumentException($"{where}: \"{k}\" is not a field of the source");
        return (b.ToImmutable(), bytes);
    }

    private static void Check(UiScalar v, string where)
    {
        if (v is UiScalar.Float64 { Value: var d } && !double.IsFinite(d)) throw new ArgumentException($"{where}: {d} is not a finite number");
        if (v is UiScalar.String { Value: null }) throw new ArgumentException($"{where}: a null text");
    }

    private static int Size(UiScalar v) => v is UiScalar.String s ? Encoding.UTF8.GetByteCount(s.Value) : 8;
}
