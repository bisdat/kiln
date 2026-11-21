// SPDX-License-Identifier: MIT
// Routines in data and on the wire. blocks/*.json: the "routines" section, a task as { "kind": "idle" }. The wire: the
// exact UTF-8 JSON kiln.routines takes (format 1), and the canonical document its content hash is made over.
//   wire       {"format":1,"key":"<mod>:<name>","planVersion":1,"contentHash":"<sha256 hex>",
//               "windows":[{"startMinute":360,"endMinute":720,"allDay":false,"priority":0,"task":{"kind":"idle"}}],
//               "fallback":{"kind":"vanilla"}}
//   canonical  {"fallback":{"kind":"vanilla"},"format":1,"planVersion":1,
//               "windows":[{"allDay":false,"endMinute":720,"priority":0,"startMinute":360,"task":{"kind":"idle"}}]}
// The canonical document has its properties in ordinal order, integers as decimal digits, no whitespace; the hash is the
// SHA-256 of its UTF-8 bytes in lower-case hex. The key and the hash itself are not part of it: a rename with Was keeps
// the hash. The owner is never in the JSON: kiln.routines takes it from the caller.
using System.Buffers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

internal static class RoutineJson
{
    public static readonly (string Section, Func<ModBlocks, string, JsonElement, BlockHandle> Make)[] Kinds =
    [
        ("routines", BlockJson.Make<RoutineBlock>),
    ];
}

/// <summary>A task's kind: the wire's names, in the order of the records.</summary>
internal enum RoutineKind : byte { Vanilla, Idle, Roam, Sleep, Sit, Inspect }

internal static class RoutineKinds
{
    private static readonly string[] Names = ["vanilla", "idle", "roam", "sleep", "sit", "inspect"];

    // Kinds the wire format reserves for later versions: each needs a capability that is off in this one.
    private static readonly (string Kind, string Capability)[] Later = [("goTo", BlockCapabilities.RoutineAnchors), ("job", BlockCapabilities.RoutineJobs)];

    public static string Name(RoutineKind kind) => Names[(int)kind];

    public static RoutineKind Of(RoutineTask task) => task switch
    {
        VanillaTask => RoutineKind.Vanilla,
        IdleTask => RoutineKind.Idle,
        RoamTask => RoutineKind.Roam,
        SleepTask => RoutineKind.Sleep,
        SitTask => RoutineKind.Sit,
        InspectTask => RoutineKind.Inspect,
        _ => throw new ArgumentException($"{task.GetType().Name} is not a routine task of this version"),
    };

    public static RoutineTask Task(RoutineKind kind) => kind switch
    {
        RoutineKind.Vanilla => new VanillaTask(),
        RoutineKind.Idle => new IdleTask(),
        RoutineKind.Roam => new RoamTask(),
        RoutineKind.Sleep => new SleepTask(),
        RoutineKind.Sit => new SitTask(),
        RoutineKind.Inspect => new InspectTask(),
        _ => throw new ArgumentOutOfRangeException(nameof(kind)),
    };

    /// <summary>The kind of a wire or data name. A name of a later version, or no kind at all, says so in <paramref name="problem"/>.</summary>
    public static bool TryParse(string? name, out RoutineKind kind, out string problem)
    {
        var i = Array.IndexOf(Names, name);
        kind = i < 0 ? default : (RoutineKind)i;
        problem = "";
        if (i >= 0) return true;
        var later = Later.FirstOrDefault(l => l.Kind == name);
        problem = later.Kind != null
            ? $"a \"{name}\" task needs {later.Capability}, which is off in this version of Kiln"
            : $"\"{name}\" is not a task; there are {string.Join(", ", Names)}";
        return false;
    }
}

// { "kind": "idle" }. Nothing else: a duration or a radius is refused, never ignored.
internal sealed class RoutineTaskConverter : JsonConverter<RoutineTask>
{
    public override RoutineTask? Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        if (r.TokenType != JsonTokenType.StartObject) throw new JsonException("a task is an object: { \"kind\": \"idle\" }");
        using var doc = JsonDocument.ParseValue(ref r);
        string? kind = null;
        foreach (var p in doc.RootElement.EnumerateObject())
        {
            if (p.Name == "kind" && p.Value.ValueKind == JsonValueKind.String) kind = p.Value.GetString();
            else if (p.Name == "kind") throw new JsonException("a task's kind is a name: \"idle\"");
            else if (p.Name is "duration" or "seconds" or "radius" or "range")
                throw new JsonException($"a task takes no \"{p.Name}\" in this version: how long an idle lasts and how far a roam goes are the villager's own");
            else throw new JsonException($"\"{p.Name}\" is not part of a task; it takes kind");
        }
        if (kind == null) throw new JsonException("a task needs a kind");
        return RoutineKinds.TryParse(kind, out var k, out var problem) ? RoutineKinds.Task(k) : throw new JsonException(problem);
    }

    public override void Write(Utf8JsonWriter w, RoutineTask v, JsonSerializerOptions o)
    {
        w.WriteStartObject();
        w.WriteString("kind", RoutineKinds.Name(RoutineKinds.Of(v)));
        w.WriteEndObject();
    }
}

/// <summary>One window of a compiled plan.</summary>
internal readonly record struct PlanWindow(int Start, int End, bool AllDay, int Priority, RoutineKind Task);

/// <summary>A plan as kiln.routines takes it: checked, immutable, with its hash and wire text.</summary>
internal sealed class RoutinePlan
{
    public const int Format = 1, MaxWindows = 64, MinutesPerDay = 1440;
    private const long NsPerMinute = 60_000_000_000;

    private readonly PlanWindow[] _windows;

    private RoutinePlan(string key, uint version, PlanWindow[] windows, RoutineKind fallback)
    {
        Key = key;
        Version = version;
        _windows = windows;
        Fallback = fallback;
        ContentHash = Convert.ToHexStringLower(SHA256.HashData(Canonical()));
        Wire = Encoding.UTF8.GetString(WriteWire());
    }

    /// <summary><c>&lt;mod&gt;:&lt;Was-or-name&gt;</c>.</summary>
    public string Key { get; }
    public uint Version { get; }
    public ReadOnlySpan<PlanWindow> Windows => _windows;
    public RoutineKind Fallback { get; }
    /// <summary>SHA-256 of <see cref="Canonical"/>, lower-case hex.</summary>
    public string ContentHash { get; }
    /// <summary>The wire document (format 1).</summary>
    public string Wire { get; }

    /// <summary>What is wrong with a plan's numbers, as (field, problem); null when it is sound. The one rule set for a
    /// block and for a wire document.</summary>
    public static (string Field, string Problem)? Check(uint version, IReadOnlyList<PlanWindow> windows, RoutineKind fallback)
    {
        if (version == 0) return ("version", "0: a plan's version is 1 or more");
        if (windows.Count > MaxWindows) return ("windows", $"{windows.Count} windows: a plan takes at most {MaxWindows}");
        for (var i = 0; i < windows.Count; i++)
        {
            var w = windows[i];
            if (w.AllDay)
            {
                if (w.Start != 0 || w.End != 0) return ($"windows[{i}]", $"allDay with {w.Start} and {w.End}: a window of the whole day has startMinute 0 and endMinute 0");
                continue;
            }
            if (w.Start is < 0 or >= MinutesPerDay) return ($"windows[{i}].startMinute", $"{w.Start} is outside 0..{MinutesPerDay - 1}");
            if (w.End is < 0 or >= MinutesPerDay) return ($"windows[{i}].endMinute", $"{w.End} is outside 0..{MinutesPerDay - 1} (midnight is 0)");
            if (w.Start == w.End) return ($"windows[{i}]", $"it starts and ends at {w.Start}: say allDay with 0 and 0 for the whole day");
        }
        if (fallback is not (RoutineKind.Vanilla or RoutineKind.Idle or RoutineKind.Roam))
            return ("fallback", $"{RoutineKinds.Name(fallback)}: the fallback is vanilla, idle or roam (it also runs when a sleep is not allowed now, so it must be a task that always can)");
        return null;
    }

    /// <summary>A checked plan. Throws <see cref="ArgumentException"/> naming the field when <see cref="Check"/> finds a problem.</summary>
    public static RoutinePlan Make(string key, uint version, IReadOnlyList<PlanWindow> windows, RoutineKind fallback)
    {
        if (Check(version, windows, fallback) is { } bad) throw new ArgumentException($"{bad.Field}: {bad.Problem}");
        return new RoutinePlan(key, version, [.. windows], fallback);
    }

    // ---- which window ---------------------------------------------------------------------------------------------

    /// <summary>The minute of the game day of a game time in nanoseconds: floor(ns / one minute) modulo 1440, also for a
    /// negative time.</summary>
    public static int MinuteOf(long nowNs)
    {
        var minutes = Math.DivRem(nowNs, NsPerMinute, out var rest);
        if (rest < 0) minutes--;
        var m = minutes % MinutesPerDay;
        return (int)(m < 0 ? m + MinutesPerDay : m);
    }

    /// <summary>Whether a window covers a minute: end-exclusive, over midnight when it starts after it ends.</summary>
    public static bool Covers(PlanWindow w, int minute) =>
        w.AllDay || (w.Start < w.End ? minute >= w.Start && minute < w.End : minute >= w.Start || minute < w.End);

    /// <summary>The window of a minute: the highest priority among those that cover it, the first written among equals.
    /// -1: none, the fallback (which is the lowest of all).</summary>
    public int Select(int minute)
    {
        var best = -1;
        for (var i = 0; i < _windows.Length; i++)
            if (Covers(_windows[i], minute) && (best < 0 || _windows[i].Priority > _windows[best].Priority))
                best = i;
        return best;
    }

    /// <summary>The task of a window index (-1: the fallback).</summary>
    public RoutineKind TaskAt(int window) => window < 0 ? Fallback : _windows[window].Task;

    // ---- the documents --------------------------------------------------------------------------------------------

    /// <summary>The canonical document the hash is made over: format, planVersion, windows and fallback, properties in
    /// ordinal order at every level, no whitespace.</summary>
    public byte[] Canonical()
    {
        var buffer = new ArrayBufferWriter<byte>();
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            WriteTask(w, "fallback", Fallback);
            w.WriteNumber("format", Format);
            w.WriteNumber("planVersion", Version);
            w.WriteStartArray("windows");
            foreach (var x in _windows)
            {
                w.WriteStartObject();
                w.WriteBoolean("allDay", x.AllDay);
                w.WriteNumber("endMinute", x.End);
                w.WriteNumber("priority", x.Priority);
                w.WriteNumber("startMinute", x.Start);
                WriteTask(w, "task", x.Task);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteEndObject();
        }
        return buffer.WrittenSpan.ToArray();
    }

    private byte[] WriteWire()
    {
        var buffer = new ArrayBufferWriter<byte>();
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            w.WriteNumber("format", Format);
            w.WriteString("key", Key);
            w.WriteNumber("planVersion", Version);
            w.WriteString("contentHash", ContentHash);
            w.WriteStartArray("windows");
            foreach (var x in _windows)
            {
                w.WriteStartObject();
                w.WriteNumber("startMinute", x.Start);
                w.WriteNumber("endMinute", x.End);
                w.WriteBoolean("allDay", x.AllDay);
                w.WriteNumber("priority", x.Priority);
                WriteTask(w, "task", x.Task);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            WriteTask(w, "fallback", Fallback);
            w.WriteEndObject();
        }
        return buffer.WrittenSpan.ToArray();
    }

    private static void WriteTask(Utf8JsonWriter w, string name, RoutineKind kind)
    {
        w.WriteStartObject(name);
        w.WriteString("kind", RoutineKinds.Name(kind));
        w.WriteEndObject();
    }

    /// <summary>Reads a wire document as strictly as kiln.routines does: format 1, every property known and present once,
    /// integers, the limits of <see cref="Check"/>, and a contentHash that is the hash of the content. The order of the
    /// properties is free. Throws <see cref="FormatException"/> saying what is wrong.</summary>
    public static RoutinePlan Parse(string wire)
    {
        JsonDocument doc;
        try { doc = JsonDocument.Parse(wire); }
        catch (JsonException e) { throw new FormatException($"a routine plan is not JSON: {e.Message}"); }
        using (doc)
        {
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) throw new FormatException("a routine plan is a JSON object");
            int? format = null;
            uint? version = null;
            string? key = null, hash = null;
            List<PlanWindow>? windows = null;
            RoutineKind? fallback = null;
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var p in root.EnumerateObject())
            {
                if (!seen.Add(p.Name)) throw new FormatException($"{p.Name} is there twice");
                switch (p.Name)
                {
                    case "format": format = Int(p.Value, "format"); break;
                    case "key": key = Str(p.Value, "key"); break;
                    case "planVersion": version = p.Value.ValueKind == JsonValueKind.Number && p.Value.TryGetUInt32(out var v) ? v : throw new FormatException("planVersion is not a whole number of 32 bits"); break;
                    case "contentHash": hash = Str(p.Value, "contentHash"); break;
                    case "windows":
                        if (p.Value.ValueKind != JsonValueKind.Array) throw new FormatException("windows is not a list");
                        if (p.Value.GetArrayLength() > MaxWindows) throw new FormatException($"{p.Value.GetArrayLength()} windows: a plan takes at most {MaxWindows}");
                        windows = [];
                        foreach (var w in p.Value.EnumerateArray()) windows.Add(Window(w, windows.Count));
                        break;
                    case "fallback": fallback = TaskOf(p.Value, "fallback"); break;
                    default: throw new FormatException($"\"{p.Name}\" is not part of a routine plan");
                }
            }
            if (format != Format) throw new FormatException(format is null ? "format is missing" : $"format {format}: this Kiln reads format {Format}");
            if (key is null || version is null || hash is null || windows is null || fallback is null)
                throw new FormatException($"{(key is null ? "key" : version is null ? "planVersion" : hash is null ? "contentHash" : windows is null ? "windows" : "fallback")} is missing");
            if (!RoutineIdentity.IsKey(key)) throw new FormatException($"key \"{key}\" is not <mod>:<name>");
            if (Check(version.Value, windows, fallback.Value) is { } bad) throw new FormatException($"{bad.Field}: {bad.Problem}");
            var plan = new RoutinePlan(key, version.Value, [.. windows], fallback.Value);
            if (!string.Equals(plan.ContentHash, hash, StringComparison.Ordinal))
                throw new FormatException($"contentHash {hash} is not the hash of the plan's content ({plan.ContentHash})");
            return plan;
        }
    }

    private static int Int(JsonElement e, string what) =>
        e.ValueKind == JsonValueKind.Number && e.TryGetInt32(out var v) ? v : throw new FormatException($"{what} is not a whole number of 32 bits");

    private static string Str(JsonElement e, string what) =>
        e.ValueKind == JsonValueKind.String ? e.GetString()! : throw new FormatException($"{what} is not a string");

    private static RoutineKind TaskOf(JsonElement e, string what)
    {
        if (e.ValueKind != JsonValueKind.Object) throw new FormatException($"{what} is not an object");
        string? kind = null;
        foreach (var p in e.EnumerateObject())
        {
            if (p.Name != "kind" || kind != null) throw new FormatException($"{what}: \"{p.Name}\" is not part of a task (it takes kind, once)");
            kind = Str(p.Value, $"{what}.kind");
        }
        if (kind is null) throw new FormatException($"{what}.kind is missing");
        return RoutineKinds.TryParse(kind, out var k, out var problem) ? k : throw new FormatException($"{what}: {problem}");
    }

    private static PlanWindow Window(JsonElement e, int index)
    {
        var at = $"windows[{index}]";
        if (e.ValueKind != JsonValueKind.Object) throw new FormatException($"{at} is not an object");
        int? start = null, end = null, priority = null;
        bool? allDay = null;
        RoutineKind? task = null;
        foreach (var p in e.EnumerateObject())
        {
            var twice = p.Name switch { "startMinute" => start != null, "endMinute" => end != null, "priority" => priority != null, "allDay" => allDay != null, "task" => task != null, _ => false };
            if (twice) throw new FormatException($"{at}.{p.Name} is there twice");
            switch (p.Name)
            {
                case "startMinute": start = Int(p.Value, $"{at}.startMinute"); break;
                case "endMinute": end = Int(p.Value, $"{at}.endMinute"); break;
                case "priority": priority = Int(p.Value, $"{at}.priority"); break;
                case "allDay": allDay = p.Value.ValueKind is JsonValueKind.True or JsonValueKind.False ? p.Value.GetBoolean() : throw new FormatException($"{at}.allDay is not true or false"); break;
                case "task": task = TaskOf(p.Value, $"{at}.task"); break;
                default: throw new FormatException($"{at}: \"{p.Name}\" is not part of a window");
            }
        }
        if (start is null || end is null || priority is null || allDay is null || task is null)
            throw new FormatException($"{at}.{(start is null ? "startMinute" : end is null ? "endMinute" : priority is null ? "priority" : allDay is null ? "allDay" : "task")} is missing");
        return new PlanWindow(start.Value, end.Value, allDay.Value, priority.Value, task.Value);
    }
}
