// SPDX-License-Identifier: MIT
//
// What Kiln's interpreter says about this start, for the Kiln menu's catalog (SfiCatalog.cs): kiln-sfi's
// kiln/state/sfi.json (schema/sfi/sfi-state.schema.json, format 1), read whole and held to the schema's bounds; and the
// Kiln-owned ShroudForge loader settings of a kiln-backend start (SfiLoaderSettings).
//
// Which start: the action bridge's rules (SfiBridge.Parse: launch.pid, launch.processStartMs within 2 s,
// launch.backend.selected), so the catalog and a button never disagree about whose file it is. A file written before
// this process began is an earlier start's. A file written during this start that cannot be read, or that breaks the
// schema, is Unknown: nothing in it is believed, and nothing falls back to ShroudForge's own folder.
//
// Threads: worker only (ModCatalog.Read, the settings writer). Nothing here runs on the render thread.
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>Whose sfi.json the state folder holds, as far as this start goes.</summary>
internal enum SfiStart
{
    /// <summary>No sfi.json: kiln-sfi has not written one (yet), or never runs here.</summary>
    None,
    /// <summary>An earlier start's file (another pid, or a reused pid's file written before this process began).</summary>
    Other,
    /// <summary>This start's file, and the frozen backend is not kiln (hosted, installed or none): kiln-sfi stands aside.</summary>
    Aside,
    /// <summary>This start's file, for the kiln backend, read whole and within the schema.</summary>
    Kiln,
    /// <summary>Written during this start but unreadable, or outside the schema: nothing in it counts.</summary>
    Unknown,
}

internal sealed record SfiDll(string Name, string State);

/// <summary>Why a selected package does not run whole this start (sfi.json <c>skip</c>).</summary>
internal sealed record SfiSkip(string Code, string Text, string Part, IReadOnlyList<string> Chain, bool Late, string? Loader);

/// <summary>A package's warning (sfi.json <c>warning</c>): presence is not-applied, data-applied-runtime-stopped or unknown.</summary>
internal sealed record SfiPackageWarning(long Revision, string Trigger, string Presence, bool SaveSensitive, string Text);

/// <summary>One package row of sfi.json.</summary>
internal sealed record SfiPackageState
{
    public required string Id { get; init; }
    public string? Version { get; init; }
    public string? Name { get; init; }
    /// <summary>selected, disabled, other-side or absent.</summary>
    public required string Intent { get; init; }
    /// <summary>not-needed, staged, prepared, committed, cached-verified, verified-restored, excluded, failed, uncertain.</summary>
    public required string Data { get; init; }
    /// <summary>not-needed, admitted, ready, active, failed, excluded, stopped.</summary>
    public required string Lua { get; init; }
    public IReadOnlyList<SfiDll> Dlls { get; init; } = [];
    /// <summary>Its data is in the game archive now; null: not known (never "not applied").</summary>
    public bool? ArchivePresent { get; init; }
    /// <summary>Its Lua runtime runs now; null: not known.</summary>
    public bool? RuntimeActive { get; init; }
    public bool SaveSensitive { get; init; }
    public SfiSkip? Skip { get; init; }
    public SfiPackageWarning? Warning { get; init; }
    /// <summary>The owner epoch of its live runtime (an integer of 1 or more), absent when it has none.</summary>
    public long? OwnerEpoch { get; init; }
}

/// <summary>This start's sfi.json in kiln mode.</summary>
internal sealed record SfiRecord(SfiClaim Claim, string Phase, string? ReasonCode, string? ReasonText, bool Incomplete,
    long WarningRevision, IReadOnlyList<SfiPackageState> Packages)
{
    private readonly Dictionary<string, SfiPackageState> _byId = Packages.ToDictionary(p => p.Id, StringComparer.Ordinal);

    public SfiPackageState? Find(string? id) => id is not null && _byId.TryGetValue(id, out var p) ? p : null;

    /// <summary>The runtime of <paramref name="id"/> takes requests now: the bridge's own rule (SfiClaim.EpochOf), so a row
    /// that says Active is a row whose button sends.</summary>
    public bool Active(string id) => Claim.EpochOf(id) is not null;
}

/// <summary>What the state folder's sfi.json says: <see cref="Record"/> for <see cref="SfiStart.Kiln"/>; for Aside, the
/// backend this start froze; for Unknown, why.</summary>
internal readonly record struct SfiStateRead(SfiStart Start, SfiRecord? Record, string? Backend, string? LaunchId, string? Why);

internal static class SfiState
{
    /// <summary>Tests: called with every path this file reads or writes.</summary>
    internal static Action<string>? OnIo;
    private const long StartSlackMs = 2000;
    internal const int MaxPackages = 4096;

    /// <summary>Worker only: sfi.json of <paramref name="src"/>'s state folder, read once with delete sharing (the writer
    /// replaces it by rename; a few tries), never through a link or from a FIFO or device.</summary>
    public static SfiStateRead Read(CatalogSources src)
    {
        var path = Path.Combine(src.StateDir, SfiBridge.StateFile);
        OnIo?.Invoke(path);
        byte[]? bytes = null;
        string? error = null;
        for (var attempt = 0; attempt < 3 && bytes is null; attempt++)
        {
            if (attempt > 0) Thread.Sleep(50);
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists)
                {
                    if (Directory.Exists(path)) { error = "it is a folder"; break; }
                    return new(SfiStart.None, null, null, null, null);
                }
                if (info.LinkTarget is not null || (info.Attributes & (FileAttributes.ReparsePoint | FileAttributes.Device)) != 0)
                {
                    error = "it is a link";
                    break;
                }
                if (info.Length <= 0 || info.Length > SfiBridge.MaxStateBytes)
                {
                    // a FIFO or socket has no length; opening one could block this worker
                    error = info.Length <= 0 ? "it is empty or not a regular file" : $"it is over {SfiBridge.MaxStateBytes} bytes";
                    break;
                }
                using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                var buf = new byte[Math.Min(fs.Length, SfiBridge.MaxStateBytes) + 1];
                var n = 0;
                for (int r; n < buf.Length && (r = fs.Read(buf, n, buf.Length - n)) > 0;) n += r;
                if (n > SfiBridge.MaxStateBytes) { error = $"it is over {SfiBridge.MaxStateBytes} bytes"; break; }
                bytes = buf[..n];
            }
            catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException)
            {
                return new(SfiStart.None, null, null, null, null);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                error = e.Message;
            }
        }
        if (bytes is not null)
        {
            try
            {
                using var doc = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 64 });
                var read = Parse(doc.RootElement, src.Pid, src.ProcessStartUtc);
                if (read.Start != SfiStart.Unknown) return read;
                error = read.Why;
            }
            catch (JsonException e) { error = "it is not JSON: " + e.Message; }
        }
        // a file this process cannot read that was written before it began is an earlier start's
        try
        {
            if (src.ProcessStartUtc != DateTime.MinValue && File.Exists(path) &&
                File.GetLastWriteTimeUtc(path) < src.ProcessStartUtc.AddMilliseconds(-StartSlackMs))
                return new(SfiStart.Other, null, null, null, null);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return new(SfiStart.Unknown, null, null, null, $"{SfiBridge.StateFile} could not be read: {error}");
    }

    /// <summary>What sfi.json says about process <paramref name="pid"/> started at <paramref name="processStartUtc"/>.
    /// Pure: tests.</summary>
    internal static SfiStateRead Parse(JsonElement d, int pid, DateTime processStartUtc)
    {
        var claim = SfiBridge.Parse(d, pid, processStartUtc);
        if (claim.Mode == SfiMode.Unknown) return new(SfiStart.Unknown, null, null, null, claim.Why);
        var launch = d.GetProperty("launch");
        var launchId = Text(launch, "launchId");
        if (claim.Mode == SfiMode.Hosted)
        {
            // another pid's file, or this start's for another backend (SfiBridge.Parse read pid and backend already)
            if (!Int(launch, "pid", out var filePid) || filePid != pid) return new(SfiStart.Other, null, null, null, null);
            var backend = launch.GetProperty("backend");
            return new(SfiStart.Aside, null, Text(backend, "selected"), launchId, null);
        }
        try
        {
            return new(SfiStart.Kiln, Record(d, claim.Claim!), "kiln", launchId, null);
        }
        catch (FormatException e)
        {
            return new(SfiStart.Unknown, null, "kiln", launchId, $"{SfiBridge.StateFile} does not match its schema: {e.Message}");
        }
    }

    private static readonly string[] Phases = ["starting", "running", "ended", "aside", "refused"];
    private static readonly string[] Intents = ["selected", "disabled", "other-side", "absent"];
    private static readonly string[] DataStates = ["not-needed", "staged", "prepared", "committed", "cached-verified", "verified-restored", "excluded", "failed", "uncertain"];
    private static readonly string[] LuaStates = ["not-needed", "admitted", "ready", "active", "failed", "excluded", "stopped"];
    private static readonly string[] DllStates = ["admitted", "loaded", "not-loaded", "refused", "failed", "retained", "foreign-owner"];
    private static readonly string[] SkipParts = ["package", "data", "lua", "dll"];
    private static readonly string[] Triggers = ["skipped", "save-sensitive-off"];
    private static readonly string[] Presences = ["not-applied", "data-applied-runtime-stopped", "unknown"];
    private static readonly string[] Bases = ["policy", "unknown-mixed", "none"];

    /// <summary>The whole record of a kiln start, or FormatException naming the first value outside the schema.</summary>
    private static SfiRecord Record(JsonElement d, SfiClaim claim)
    {
        var phase = OneOf(d, "phase", Phases);
        var incomplete = Bool(d, "incomplete");
        if (incomplete != (phase is "starting" or "running")) throw new FormatException($"incomplete is {incomplete} in phase {phase}");
        // a kiln record is never aside (aside means the frozen backend is not kiln)
        if (phase == "aside") throw new FormatException("phase aside for the kiln backend");
        string? reasonCode = null, reasonText = null;
        if (d.TryGetProperty("reason", out var reason) && reason.ValueKind != JsonValueKind.Null)
        {
            reasonCode = Code(reason, "code");
            reasonText = Bounded(reason, "text", 4096, min: 1);
        }
        if (phase == "refused" && reasonText is null) throw new FormatException("phase refused without a reason");
        var revision = Long(d, "warningRevision", 0);
        if (!d.TryGetProperty("packages", out var list) || list.ValueKind != JsonValueKind.Array) throw new FormatException("packages is not an array");
        if (list.GetArrayLength() > MaxPackages) throw new FormatException($"more than {MaxPackages} packages");
        var packages = new List<SfiPackageState>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var p in list.EnumerateArray())
        {
            var row = Package(p, revision);
            if (!seen.Add(row.Id)) throw new FormatException($"package {row.Id} is listed twice");
            packages.Add(row);
        }
        return new SfiRecord(claim, phase, reasonCode, reasonText, incomplete, revision, packages);
    }

    private static SfiPackageState Package(JsonElement p, long revision)
    {
        if (p.ValueKind != JsonValueKind.Object) throw new FormatException("a package is not an object");
        var id = Text(p, "id");
        if (!SfiBridgeFiles.SfId(id)) throw new FormatException("a package has no valid id");
        string Where(string what) => $"package {id}: {what}";
        var data = OneOf(p, "data", DataStates, id);
        var lua = OneOf(p, "lua", LuaStates, id);
        var archive = NullableBool(p, "archivePresent", id);
        var runtime = NullableBool(p, "runtimeActive", id);
        if ((lua == "active") != (runtime == true)) throw new FormatException(Where($"lua {lua} with runtimeActive {runtime?.ToString() ?? "null"}"));
        if (data is "committed" or "cached-verified" && archive != true) throw new FormatException(Where($"data {data} without archivePresent true"));
        if (data == "uncertain" && archive is not null) throw new FormatException(Where("data uncertain with a known archivePresent"));
        if (!p.TryGetProperty("dlls", out var dl) || dl.ValueKind != JsonValueKind.Array || dl.GetArrayLength() > 64) throw new FormatException(Where("dlls"));
        var dlls = new List<SfiDll>();
        foreach (var x in dl.EnumerateArray())
        {
            if (x.ValueKind != JsonValueKind.Object) throw new FormatException(Where("a dll is not an object"));
            dlls.Add(new SfiDll(Bounded(x, "name", 260, min: 1, id), OneOf(x, "state", DllStates, id)));
        }
        if (!p.TryGetProperty("saveSensitive", out var ss) || ss.ValueKind != JsonValueKind.Object) throw new FormatException(Where("saveSensitive"));
        var sensitive = Bool(ss, "value", id);
        var basis = OneOf(ss, "basis", Bases, id);
        if (sensitive != (basis != "none")) throw new FormatException(Where($"saveSensitive {sensitive} with basis {basis}"));
        SfiSkip? skip = null;
        if (Member(p, "skip", id) is { ValueKind: JsonValueKind.Object } s)
        {
            if (!s.TryGetProperty("chain", out var ch) || ch.ValueKind != JsonValueKind.Array || ch.GetArrayLength() > 64) throw new FormatException(Where("skip chain"));
            var chain = new List<string>();
            foreach (var c in ch.EnumerateArray())
                chain.Add(c.ValueKind == JsonValueKind.String && SfiBridgeFiles.SfId(c.GetString()) ? c.GetString()! : throw new FormatException(Where("skip chain id")));
            string? loader = null;
            if (Member(s, "loader", id) is { ValueKind: JsonValueKind.String } l) loader = Code(s, "loader", id, 32);
            skip = new SfiSkip(Code(s, "code", id), Bounded(s, "text", 4096, min: 1, id), OneOf(s, "part", SkipParts, id), chain, Bool(s, "late", id), loader);
        }
        SfiPackageWarning? warning = null;
        if (Member(p, "warning", id) is { ValueKind: JsonValueKind.Object } w)
        {
            var wr = Long(w, "revision", 1, id);
            if (wr > revision) throw new FormatException(Where("a warning revision above warningRevision"));
            warning = new SfiPackageWarning(wr, OneOf(w, "trigger", Triggers, id), OneOf(w, "presence", Presences, id), Bool(w, "saveSensitive", id), Bounded(w, "text", 8192, min: 1, id));
            if (warning.SaveSensitive != sensitive || warning.Trigger == "save-sensitive-off" && !sensitive)
                throw new FormatException(Where("warning saveSensitive differs from the package's"));
            if (warning.Presence == "not-applied" && archive != false || warning.Presence == "data-applied-runtime-stopped" && (archive != true || runtime != false) ||
                warning.Presence == "unknown" && archive is not null)
                throw new FormatException(Where($"warning presence {warning.Presence} with archivePresent {archive?.ToString() ?? "null"}"));
        }
        var intent = OneOf(p, "intent", Intents, id);
        if (intent != "selected" && skip is not null) throw new FormatException(Where($"a skip on a package with intent {intent}"));
        // selected: a skip and its "skipped" warning go together; any other intent never warns "skipped", other-side never warns
        if (intent == "selected" && (skip is not null) != (warning is not null) ||
            warning is not null && warning.Trigger != (intent == "selected" ? "skipped" : "save-sensitive-off") ||
            intent == "other-side" && warning is not null)
            throw new FormatException(Where($"skip and warning do not fit intent {intent}"));
        long? epoch = null;
        if (p.TryGetProperty("ownerEpoch", out _)) epoch = Long(p, "ownerEpoch", 1, id);
        return new SfiPackageState
        {
            Id = id!, Version = NullableBounded(p, "version", 64, id), Name = NullableBounded(p, "name", 200, id, min: 0), Intent = intent, Data = data, Lua = lua,
            Dlls = dlls, ArchivePresent = archive, RuntimeActive = runtime, SaveSensitive = sensitive, Skip = skip, Warning = warning, OwnerEpoch = epoch,
        };
    }

    private static string Prefix(string? id) => id is null ? "" : $"package {id}: ";

    private static JsonElement? Member(JsonElement e, string name, string? id = null)
    {
        if (!e.TryGetProperty(name, out var v)) throw new FormatException($"{Prefix(id)}{name} is missing");
        return v.ValueKind == JsonValueKind.Null ? null : v;
    }

    private static string? Text(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private static string Bounded(JsonElement e, string name, int max, int min = 0, string? id = null) =>
        Text(e, name) is { } s && s.Length >= min && s.Length <= max ? s : throw new FormatException($"{Prefix(id)}{name} is not a text of {min} to {max} characters");

    private static string? NullableBounded(JsonElement e, string name, int max, string? id = null, int min = 1) =>
        Member(e, name, id) is null ? null : Bounded(e, name, max, min, id);

    private static string OneOf(JsonElement e, string name, string[] allowed, string? id = null) =>
        Text(e, name) is { } s && allowed.Contains(s) ? s : throw new FormatException($"{Prefix(id)}{name} is not one of {string.Join(", ", allowed)}");

    private static string Code(JsonElement e, string name, string? id = null, int max = 64) =>
        Text(e, name) is { Length: > 0 } s && s.Length <= max && (char.IsAsciiLetterLower(s[0]) || char.IsAsciiDigit(s[0])) &&
        s.All(c => char.IsAsciiLetterLower(c) || char.IsAsciiDigit(c) || c is '.' or '-')
            ? s : throw new FormatException($"{Prefix(id)}{name} is not a code");

    private static bool Bool(JsonElement e, string name, string? id = null) =>
        e.TryGetProperty(name, out var v) && v.ValueKind is JsonValueKind.True or JsonValueKind.False ? v.GetBoolean()
            : throw new FormatException($"{Prefix(id)}{name} is not true or false");

    private static bool? NullableBool(JsonElement e, string name, string? id = null) =>
        Member(e, name, id) is null ? null : Bool(e, name, id);

    private static long Long(JsonElement e, string name, long min, string? id = null) =>
        Int(e, name, out var v) && v >= min ? v : throw new FormatException($"{Prefix(id)}{name} is not an integer of at least {min}");

    private static bool Int(JsonElement e, string name, out long value)
    {
        value = 0;
        if (!e.TryGetProperty(name, out var v) || v.ValueKind != JsonValueKind.Number) return false;
        var raw = v.GetRawText();
        return raw.IndexOfAny(['.', 'e', 'E']) < 0 && v.TryGetInt64(out value);
    }

    internal static void ResetForTests() => OnIo = null;
}

/// <summary>The Kiln-owned ShroudForge loader settings of a kiln-backend start:
/// &lt;game&gt;/kiln/config/sfi/loader.json, beside the compatibility rules' compatibility/rules.json. It holds
/// ShroudForge's loader document shape for the part the interpreter reads (<c>modules.network</c>: enabled, serverSteamId,
/// allowedClientSteamIds; loader.schema.json:757-773), so kiln-sfi's settings snapshot takes it as it is. No file means
/// ShroudForge's defaults (loader.default.json:278-282): nothing is written until the player changes a value, and
/// &lt;game&gt;/shroudforge is never read or written. Importing hosted ShroudForge's values is an explicit step at the
/// backend switch (the launcher), never automatic.</summary>
internal static class SfiLoaderSettings
{
    internal const int MaxBytes = 64 << 10;
    public static readonly string[] Keys = ["enabled", "serverSteamId", "allowedClientSteamIds"];

    public static string PathOf(string kilnRoot) => Path.Combine(kilnRoot, "config", "sfi", "loader.json");

    /// <summary>ShroudForge's default network block, as the document Kiln writes.</summary>
    public static JsonObject Defaults() => new()
    {
        ["schemaVersion"] = 1,
        ["modules"] = new JsonObject
        {
            ["network"] = new JsonObject { ["enabled"] = true, ["serverSteamId"] = "", ["allowedClientSteamIds"] = "" },
        },
    };

    /// <summary>The network values now (ShroudForge's defaults when there is no file), or why the file cannot be used: the
    /// interpreter then answers "Network module settings are unavailable", and the page offers no edit over it.</summary>
    public static (JsonObject? Network, bool Exists, string? Error) Read(string kilnRoot)
    {
        var path = PathOf(kilnRoot);
        SfiState.OnIo?.Invoke(path);
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists) return ((JsonObject)Defaults()["modules"]!["network"]!.DeepClone(), false, null);
            if (Linked(path)) return (null, true, "the settings file is a link");
            if (info.Length <= 0 || info.Length > MaxBytes) return (null, true, "the settings file is empty or too large");
            using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var bytes = new byte[MaxBytes + 1];
            var n = 0;
            for (int r; n < bytes.Length && (r = f.Read(bytes, n, bytes.Length - n)) > 0;) n += r;
            if (n > MaxBytes) return (null, true, "the settings file is too large");
            var doc = JsonNode.Parse(bytes.AsSpan(0, n), new JsonNodeOptions(), new JsonDocumentOptions { MaxDepth = 64 }) as JsonObject;
            if (doc is null || doc["schemaVersion"] is not JsonValue sv || sv.GetValueKind() != JsonValueKind.Number || sv.ToJsonString() != "1")
                return (null, true, "the settings file is not schemaVersion 1");
            var network = doc["modules"]?["network"] as JsonObject ?? new JsonObject();
            var merged = (JsonObject)Defaults()["modules"]!["network"]!.DeepClone();
            foreach (var (key, value) in network)
            {
                if (!Keys.Contains(key)) return (null, true, $"the settings file has an unknown network setting {key}");
                if (Check(key, value) is { } why) return (null, true, why);
                merged[key] = value?.DeepClone();
            }
            return (merged, true, null);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException)
        {
            return (null, true, "the settings file cannot be read: " + e.Message);
        }
    }

    /// <summary>Why ShroudForge's schema refuses <paramref name="value"/> for network setting <paramref name="key"/>, or null.</summary>
    public static string? Check(string key, JsonNode? value)
    {
        var kind = value is JsonValue v ? v.GetValueKind() : JsonValueKind.Undefined;
        switch (key)
        {
            case "enabled":
                return kind is JsonValueKind.True or JsonValueKind.False ? null : "enabled must be on or off";
            case "serverSteamId":
                if (kind != JsonValueKind.String) return "the server's Steam id must be text";
                var id = value!.GetValue<string>();
                return id.Length == 0 || id.Length is >= 16 and <= 20 && id.All(char.IsAsciiDigit) ? null : "the server's Steam id is 16 to 20 digits, or empty";
            case "allowedClientSteamIds":
                if (kind != JsonValueKind.String) return "the allowed players must be text";
                return value!.GetValue<string>().Length <= 4096 ? null : "the allowed players list is longer than 4096 characters";
            default:
                return $"{key} is not a network setting";
        }
    }

    /// <summary>Worker only: sets one network value. A refused value or an unusable file leaves the file as it was; the first
    /// change writes ShroudForge's defaults with it. Replaced whole (unique temp file in the same folder, then rename).</summary>
    public static JsonObject Set(string kilnRoot, string key, JsonNode? value)
    {
        if (Check(key, value) is { } why) throw new SfEditException(why);
        var (network, exists, error) = Read(kilnRoot);
        if (error is not null || network is null) throw new SfEditException(error ?? "the settings file cannot be read");
        var path = PathOf(kilnRoot);
        var dir = Path.GetDirectoryName(path)!;
        JsonObject doc;
        if (exists)
        {
            var bytes = File.ReadAllBytes(path);
            doc = JsonNode.Parse(bytes, new JsonNodeOptions(), new JsonDocumentOptions { MaxDepth = 64 }) as JsonObject ?? throw new SfEditException("the settings file changed while it was saved");
        }
        else doc = Defaults();
        if (doc["modules"] is not JsonObject modules) doc["modules"] = modules = new JsonObject();
        if (modules["network"] is not JsonObject net) modules["network"] = net = new JsonObject();
        foreach (var k in Keys)
            if (!net.ContainsKey(k)) net[k] = network[k]?.DeepClone();
        net[key] = value?.DeepClone();
        // the folders are checked before anything is made in them: a linked kiln/config or config/sfi gets no new folder
        // inside its target either, and once more after (one made meanwhile)
        var parent = Path.GetDirectoryName(dir)!;
        if (Linked(dir) || Linked(parent) || File.Exists(path) && Linked(path)) throw new SfEditException("the settings folder is a link");
        Directory.CreateDirectory(dir);
        if (Linked(dir) || Linked(parent)) throw new SfEditException("the settings folder is a link");
        var temp = Path.Combine(dir, $".loader.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp");
        SfiState.OnIo?.Invoke(temp);
        try
        {
            using (var f = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                f.Write(System.Text.Encoding.UTF8.GetBytes(doc.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n"));
                f.Flush(true);
            }
            SfiState.OnIo?.Invoke(path);
            File.Move(temp, path, overwrite: true);
        }
        finally
        {
            try { if (File.Exists(temp)) File.Delete(temp); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return (JsonObject)net.DeepClone();
    }

    private static bool Linked(string path)
    {
        try
        {
            var i = new FileInfo(path);
            if (!i.Exists && !Directory.Exists(path)) return false;
            return i.LinkTarget is not null || (i.Attributes & FileAttributes.ReparsePoint) != 0;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return true; }
    }
}
