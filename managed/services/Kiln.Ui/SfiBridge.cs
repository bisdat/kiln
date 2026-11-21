// SPDX-License-Identifier: MIT
//
// The Kiln menu's ShroudForge action buttons in a kiln-backend start (Kiln's interpreter runs the ShroudForge mods):
// which start this is, read from kiln-sfi's kiln/state/sfi.json (schema/sfi/sfi-state.schema.json), and the request
// handed to the interpreter's UI bridge (SfiBridgeFiles.cs), not to ShroudForge's ui/actions folder.
//
// Which backend: sfi.json names this process (launch.pid and launch.processStartMs) and launch.backend.selected is
// "kiln". Missing, another pid's, or this start with another backend: the hosted path runs, unchanged
// (ShroudForgeUi.QueueAction). Unreadable, or this pid with another process start, but written during this start:
// nothing is sent anywhere, as a request left in ShroudForge's folder would run at a later hosted start (the same
// file written before this process began is a reused pid's: hosted).
//
// Sent means enqueued: only SfiQueueStatus.Queued says "Requested". A package without a live runtime (no ownerEpoch,
// runtimeActive not true, the interpreter not starting or running), a request of its still waiting, a full or unusable
// folder: not sent, and the line says so.
//
// Threads: Start is the only call the render thread makes (no IO); Send and ReadClaim read and write files, on a worker.
using System.Diagnostics.CodeAnalysis;
using System.Text.Json;

namespace Kiln;

/// <summary>Who answers ShroudForge's mods this start, as far as sfi.json tells.</summary>
internal enum SfiMode
{
    /// <summary>No sfi.json of this start for the kiln backend: ShroudForge's own runtime (hosted or installed), if any.</summary>
    Hosted,
    /// <summary>sfi.json says this process runs Kiln's interpreter.</summary>
    Kiln,
    /// <summary>sfi.json was written during this start but cannot be read: nothing may be sent either way.</summary>
    Unknown,
}

/// <summary>One package row of sfi.json as an action needs it. <see cref="OwnerEpoch"/> is null when the package has no
/// live runtime (the member is absent), or when the row is not usable.</summary>
internal readonly record struct SfiPackageLive(bool RuntimeActive, long? OwnerEpoch);

/// <summary>sfi.json of this start in kiln mode: the launch whose bridge folder takes requests, and its packages.</summary>
internal sealed record SfiClaim(SfiLaunch Launch, string Phase, IReadOnlyDictionary<string, SfiPackageLive> Packages)
{
    /// <summary>The interpreter starts or runs (ended, refused: it runs nothing more this start).</summary>
    public bool Live => Phase is "starting" or "running";

    /// <summary>The owner epoch of <paramref name="id"/>'s live runtime, or null when nothing would take a request of it.</summary>
    public long? EpochOf(string id) =>
        Live && Packages.TryGetValue(id, out var p) && p.RuntimeActive && p.OwnerEpoch is >= 1 ? p.OwnerEpoch : null;
}

internal readonly record struct SfiClaimRead(SfiMode Mode, SfiClaim? Claim, string? Why);

internal static class SfiBridge
{
    internal const string StateFile = "sfi.json";
    /// <summary>The largest sfi.json read (4096 packages with their texts stay far below it).</summary>
    internal const int MaxStateBytes = 32 << 20;
    /// <summary>A file written this close before the process start may still be this start's (clocks, as ModCatalog).</summary>
    private const long StartSlackMs = 2000;

    /// <summary>Tests: called with what is read or written each time this file does IO.</summary>
    internal static Action<string>? OnIo;
    /// <summary>The cleanup of ended launches' folders (tests replace it).</summary>
    internal static Func<string, string?, IReadOnlyList<SfiCleanupOutcome>> Cleanup = SfiBridgeFiles.Cleanup;
    internal static Func<string, SfiLaunch, string, long, string, string?, SfiQueueResult> Enqueue = SfiBridgeFiles.EnqueueAction;
    internal static Func<string, string, string, int> Pending = SfiBridgeFiles.PendingActions;
    private static int _started;
    private static volatile Task? _startTask;

    /// <summary>Once per process, from any thread (the render thread too: no IO here): prunes ended launches' bridge
    /// folders on a worker (SfiBridgeFiles.Cleanup), keeping this start's.</summary>
    public static void Start()
    {
        if (Volatile.Read(ref _started) != 0 || Interlocked.Exchange(ref _started, 1) != 0) return;
        _startTask = Task.Run(() =>
        {
            try
            {
                var src = ModCatalog.Sources();
                var read = ReadClaim(src);
                if (!UsableGameDir(src.GameDir)) return;
                OnIo?.Invoke("cleanup");
                var outcomes = Cleanup(src.GameDir, read.Claim?.Launch.LaunchId);
                var removed = outcomes.Count(o => o.Removed);
                if (removed > 0) UiEngine.Log(LogLevel.Info, $"Kiln's interpreter: removed {removed} ended launch folder(s)");
            }
            catch (Exception e)  // housekeeping: a failure only logs
            {
                UiEngine.Log(LogLevel.Warn, "Kiln's interpreter: cleanup of ended launches failed: " + e.Message);
            }
        });
    }

    /// <summary>Tests: the cleanup Start began, if any.</summary>
    internal static Task? StartTask => _startTask;

    /// <summary>Worker only. The request for <paramref name="p"/>'s action <paramref name="action"/> in a kiln-backend
    /// start: its line for the page and whether it was sent; null when this start is not one (the hosted path goes on).</summary>
    public static (Func<string> Text, bool Ok)? Send(SfPackage p, string action, string label)
    {
        CatalogSources src;
        SfiClaimRead read;
        try
        {
            src = ModCatalog.Sources();
            read = ReadClaim(src);
        }
        catch (Exception e)
        {
            // which backend runs is not known: neither folder gets a request
            var message = e.Message;
            UiEngine.Log(LogLevel.Warn, $"ShroudForge mod {p.Id}: action {action} not sent: {message}");
            return (() => KilnText.T("shroudforge.action.failed", ("action", label), ("error", message)), false);
        }
        if (read.Mode == SfiMode.Hosted) return null;
        if (read.Mode == SfiMode.Unknown || read.Claim is not { } claim)
        {
            var why = read.Why ?? "the interpreter's state could not be read";
            UiEngine.Log(LogLevel.Warn, $"ShroudForge mod {p.Id}: action {action} not sent: {why}");
            return (() => KilnText.T("shroudforge.action.failed", ("action", label), ("error", why)), false);
        }
        if (claim.EpochOf(p.Id) is not { } epoch)
            return (() => KilnText.T("shroudforge.action.not_running", ("action", label)), false);
        try
        {
            var game = src.GameDir;
            string? refused = !p.Groups.Any(g => g.Actions.Any(a => a.Id == action)) ? $"{p.Name} declares no action {action}"
                : !UsableGameDir(game) ? "the game folder cannot be used"
                : null;
            if (refused is not null)
            {
                UiEngine.Log(LogLevel.Warn, $"ShroudForge mod {p.Id}: action {action} not sent: {refused}");
                return (() => KilnText.T("shroudforge.action.failed", ("action", label), ("error", refused)), false);
            }
            // as the hosted menu: one of Kiln's requests per mod at a time (the runtime drains a few per turn)
            OnIo?.Invoke("pending");
            if (Pending(game, claim.Launch.LaunchId, p.Id) > 0)
                return (() => KilnText.T("shroudforge.action.still_waiting", ("action", label), ("mod", p.Name)), false);
            OnIo?.Invoke("enqueue");
            var r = Enqueue(game, claim.Launch, p.Id, epoch, action, null);
            switch (r.Status)
            {
                case SfiQueueStatus.Queued:
                    UiEngine.Log(LogLevel.Info, $"ShroudForge mod {p.Id}: action {action} requested from the Kiln menu (Kiln's interpreter, epoch {epoch})");
                    return (() => KilnText.T("shroudforge.action.requested", ("action", label)), true);
                case SfiQueueStatus.Full:
                    return (() => KilnText.T("shroudforge.action.still_waiting", ("action", label), ("mod", p.Name)), false);
                default:
                    var reason = r.Reason;
                    UiEngine.Log(LogLevel.Warn, $"ShroudForge mod {p.Id}: action {action} not sent ({r.Status}): {reason}");
                    return (() => KilnText.T("shroudforge.action.failed", ("action", label), ("error", reason)), false);
            }
        }
        catch (Exception e)  // whatever the files hold: the player gets a line, not a silent worker
        {
            var message = e.Message;
            UiEngine.Log(LogLevel.Warn, $"ShroudForge mod {p.Id}: action {action} could not be sent: {message}");
            return (() => KilnText.T("shroudforge.action.failed", ("action", label), ("error", message)), false);
        }
    }

    /// <summary>A folder the bridge can take: a full path no file call refuses, and there.</summary>
    internal static bool UsableGameDir([NotNullWhen(true)] string? game)
    {
        try
        {
            return !string.IsNullOrEmpty(game) && game.IndexOfAny(Path.GetInvalidPathChars()) < 0 && game.IndexOf('\0') < 0 &&
                   Path.IsPathFullyQualified(game) && Directory.Exists(game);
        }
        catch (Exception e) when (e is ArgumentException or NotSupportedException or IOException or UnauthorizedAccessException) { return false; }
    }

    /// <summary>Worker only: sfi.json of <paramref name="src"/>'s state folder, read with delete sharing and retried a
    /// few times (the writer replaces it by rename).</summary>
    public static SfiClaimRead ReadClaim(CatalogSources src)
    {
        var path = Path.Combine(src.StateDir, StateFile);
        OnIo?.Invoke("state");
        byte[]? bytes = null;
        string? error = null;
        for (var attempt = 0; attempt < 3 && bytes is null; attempt++)
        {
            if (attempt > 0) Thread.Sleep(50);
            try
            {
                using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                if (fs.Length > MaxStateBytes)
                {
                    error = $"{StateFile} is over {MaxStateBytes} bytes";
                    break;
                }
                var buf = new byte[fs.Length];
                var n = 0;
                for (int r; n < buf.Length && (r = fs.Read(buf, n, buf.Length - n)) > 0;) n += r;
                bytes = n == buf.Length ? buf : buf[..n];
            }
            catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException)
            {
                return new(SfiMode.Hosted, null, null);
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
                if (read.Mode != SfiMode.Unknown) return read;
                error = read.Why;
            }
            catch (JsonException e) { error = $"{StateFile} is not JSON: {e.Message}"; }
        }
        // an unreadable file from before this process started is an earlier start's: not this start's backend
        try
        {
            if (src.ProcessStartUtc != DateTime.MinValue && File.Exists(path) &&
                File.GetLastWriteTimeUtc(path) < src.ProcessStartUtc.AddMilliseconds(-StartSlackMs))
                return new(SfiMode.Hosted, null, null);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return new(SfiMode.Unknown, null, $"{StateFile} could not be read: {error}");
    }

    /// <summary>What sfi.json says about process <paramref name="pid"/> started at <paramref name="processStartUtc"/>
    /// (MinValue: not known, the pid alone decides). Pure: tests.</summary>
    internal static SfiClaimRead Parse(JsonElement d, int pid, DateTime processStartUtc)
    {
        static SfiClaimRead Unknown(string why) => new(SfiMode.Unknown, null, why);
        if (d.ValueKind != JsonValueKind.Object) return Unknown($"{StateFile} is not an object");
        if (!d.TryGetProperty("launch", out var launch) || launch.ValueKind != JsonValueKind.Object)
            return Unknown($"{StateFile} has no launch");
        // another start's file says nothing about this one
        if (!Int(launch, "pid", out var filePid)) return Unknown($"{StateFile} names no process");
        if (filePid != pid) return new(SfiMode.Hosted, null, null);
        if (!Int(launch, "processStartMs", out var startMs)) return Unknown($"{StateFile} names no process start");
        // this pid with another start: a reused pid (its file was written before this process began) or a side whose start
        // time fell back to the first moment it asked (the file is this start's). ReadClaim's file-time rule tells them
        // apart; until then neither folder may get a request
        if (processStartUtc != DateTime.MinValue &&
            Math.Abs(new DateTimeOffset(DateTime.SpecifyKind(processStartUtc, DateTimeKind.Utc)).ToUnixTimeMilliseconds() - startMs) > StartSlackMs)
            return Unknown($"{StateFile} names this process with another process start");
        if (!launch.TryGetProperty("backend", out var backend) || backend.ValueKind != JsonValueKind.Object ||
            Text(backend, "selected") is not { } selected)
            return Unknown($"{StateFile} names no backend");
        if (selected != "kiln") return new(SfiMode.Hosted, null, null);
        if (!Int(d, "format", out var format) || format != 1) return Unknown($"{StateFile} is not format 1");
        var launchId = Text(launch, "launchId");
        var planHash = Text(launch, "planHash");
        if (!SfiBridgeFiles.LaunchId(launchId) || planHash is not { Length: 64 } || !planHash.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f') ||
            filePid is < 1 or > int.MaxValue || startMs < 0)
            return Unknown($"{StateFile} has no usable launch identity");
        if (Text(d, "phase") is not { } phase) return Unknown($"{StateFile} has no phase");
        var packages = new Dictionary<string, SfiPackageLive>(StringComparer.Ordinal);
        var twice = new HashSet<string>(StringComparer.Ordinal);
        if (d.TryGetProperty("packages", out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var p in list.EnumerateArray())
            {
                if (p.ValueKind != JsonValueKind.Object || Text(p, "id") is not { } id) continue;
                var active = p.TryGetProperty("runtimeActive", out var ra) && ra.ValueKind == JsonValueKind.True &&
                             Text(p, "intent") == "selected";
                long? epoch = p.TryGetProperty("ownerEpoch", out _) && Int(p, "ownerEpoch", out var e) && e >= 1 ? e : null;
                if (!packages.TryAdd(id, new SfiPackageLive(active, epoch))) twice.Add(id);
            }
        // a package listed twice: which row is its runtime is not known, so neither is
        foreach (var id in twice) packages[id] = new SfiPackageLive(false, null);
        return new(SfiMode.Kiln, new SfiClaim(new SfiLaunch(launchId, planHash, (int)filePid, startMs), phase, packages), null);
    }

    private static string? Text(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private static bool Int(JsonElement e, string name, out long value)
    {
        value = 0;
        if (!e.TryGetProperty(name, out var v) || v.ValueKind != JsonValueKind.Number) return false;
        var raw = v.GetRawText();
        return raw.IndexOfAny(['.', 'e', 'E']) < 0 && v.TryGetInt64(out value);
    }

    internal static void ResetForTests()
    {
        try { _startTask?.Wait(TimeSpan.FromSeconds(10)); }
        catch (AggregateException) { }
        _startTask = null;
        Interlocked.Exchange(ref _started, 0);
        OnIo = null;
        Cleanup = SfiBridgeFiles.Cleanup;
        Enqueue = SfiBridgeFiles.EnqueueAction;
        Pending = SfiBridgeFiles.PendingActions;
    }
}
