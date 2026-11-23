// SPDX-License-Identifier: MIT
// server_restart / server_restart_cancel (docs/kiln/launcher/admin-protocol.md section 8, "Restart"): a countdown in chat,
// the world save, then the server's own stop, the one Ctrl-C starts (mods unload, the game saves and exits 0). Before
// the stop Kiln.Admin writes kiln/data/kiln-admin/restart.json; `kilnctl run` reads it when the server has exited and
// then exits 75 itself, which kiln@.service (Restart=on-failure) restarts. Elsewhere the server just stops:
// restart.supported says "unknown" there. Never a hard kill: when the stop cannot be raised, the server keeps running
// (saved) and the op's state says why.
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace Kiln.Admin;

/// <summary>Whether a stop comes back by itself. kilnctl run sets <c>KILN_RUN_ID</c> for every server it runs and
/// <c>KILN_RESTART=kilnctl</c> when it is a systemd unit's main process (kiln@.service restarts on its exit code 75).</summary>
internal sealed record RestartSupport(bool Supported, string? Via, string? RunId)
{
    /// <summary>What `kilnctl run` exits with after a requested restart (EX_TEMPFAIL; not one of kiln@.service's
    /// RestartPreventExitStatus 1 2 3).</summary>
    public const int KilnctlExitCode = 75;

    public static RestartSupport Detect(Func<string, string?> env)
    {
        var run = env("KILN_RUN_ID") is { Length: > 0 and <= 64 } r ? r : null;
        return env("KILN_RESTART") == "kilnctl" && run is not null ? new(true, "kilnctl", run) : new(false, null, run);
    }

    public static RestartSupport FromEnvironment() => Detect(Environment.GetEnvironmentVariable);

    /// <summary>{supported: "supported" | "unknown", via: "kilnctl" | null, code: 75 | null}.</summary>
    public JsonObject ToJson() => new()
    {
        ["supported"] = Supported ? "supported" : "unknown", ["via"] = Via, ["code"] = Supported ? KilnctlExitCode : null,
    };
}

/// <summary>What the restart needs from the server. AdminMod's is the real one; tests have their own.</summary>
internal interface IRestartHost
{
    /// <summary>Monotonic milliseconds.</summary>
    long NowMs { get; }

    /// <summary>A chat line to everyone (nothing without Kiln.Chat).</summary>
    void Broadcast(string text);

    /// <summary>The world save, then the marker file; null when both are done, else why not. Runs off the tick thread
    /// apart from its start.</summary>
    Task<string?> SaveAndMark(JsonObject marker);

    /// <summary>Removes the marker again (a failed stop).</summary>
    void DeleteMarker();

    /// <summary>Raises the server's stop. Attempt 1: kernelbase!CtrlRoutine on a new thread (what Wine's SIGINT and a
    /// console's Ctrl-C run); 2: GenerateConsoleCtrlEvent. Returns how, or null when that way is not available.</summary>
    string? RaiseStop(int attempt);

    void Log(LogLevel level, string text);
}

internal enum RestartState
{
    Counting,
    Saving,
    Stopping,
    Failed,
    Canceled,
}

/// <summary>One requested restart. Tick thread: <see cref="Tick"/> every step drives it.</summary>
internal sealed class RestartController(IRestartHost host, RestartSupport support)
{
    /// <summary>Seconds before the restart at which chat is told again (with the first announcement).</summary>
    public static readonly int[] Marks = [600, 300, 120, 60, 30, 10, 5, 4, 3, 2, 1];
    public const int MaxSeconds = 3600, DefaultSeconds = 60, MaxMessage = 200, MaxBy = 120;
    /// <summary>How long one way of raising the stop gets to start it.</summary>
    public const int StopWaitMs = 10_000;

    private long _at, _deadline;
    private int _announced, _attempt;
    private Task<string?>? _saving;
    private string _by = "", _message = "", _requested = "";
    private string? _error, _how;

    public RestartSupport Support => support;
    public RestartState? State { get; private set; }

    /// <summary>A restart is counting down, saving or stopping.</summary>
    public bool Busy => State is RestartState.Counting or RestartState.Saving or RestartState.Stopping;

    /// <summary>The stop was raised for a restart (TcpAdmin's bye reason then says "restart").</summary>
    public bool Stopping => State == RestartState.Stopping;

    private string Verb => support.Supported ? "restarts" : "stops";

    /// <summary>server_restart. Throws <see cref="AdminError"/> while one is already under way.</summary>
    public JsonObject Schedule(int seconds, string? message, string by)
    {
        if (Busy) throw new AdminError($"a restart is already {Describe()} (server_restart_cancel first)");
        if (seconds < 0 || seconds > MaxSeconds) throw new AdminError($"seconds: 0 to {MaxSeconds}");
        _at = host.NowMs + seconds * 1000L;
        _by = by;
        _message = message ?? "";
        _requested = DateTimeOffset.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'");
        _announced = int.MaxValue;
        _attempt = 0;
        _error = _how = null;
        _saving = null;
        State = RestartState.Counting;
        host.Log(LogLevel.Info, $"restart requested by {by}: the server {Verb} in {seconds} s{(support.Supported ? $" ({support.Via} brings it back)" : " (nothing known brings it back)")}");
        if (seconds > 0)
        {
            host.Broadcast(Line($"The server {Verb} in {Span(seconds)}."));
            _announced = seconds;
        }
        Tick();
        return Info()!;
    }

    /// <summary>server_restart_cancel: only while counting down.</summary>
    public JsonObject Cancel(string by)
    {
        if (State != RestartState.Counting)
            throw new AdminError(Busy ? $"too late to cancel: the restart is {Describe()}" : "no restart is scheduled");
        State = RestartState.Canceled;
        host.Log(LogLevel.Info, $"restart canceled by {by}");
        host.Broadcast($"The server {(support.Supported ? "restart" : "stop")} was canceled.");
        return new JsonObject { ["canceled"] = true };
    }

    /// <summary>Tick thread, every step.</summary>
    public void Tick()
    {
        switch (State)
        {
            case RestartState.Counting:
            {
                var left = _at - host.NowMs;
                if (left > 0)
                {
                    var s = (int)((left + 999) / 1000);
                    var mark = Marks.Where(m => m >= s && m < _announced).DefaultIfEmpty(0).Min();
                    if (mark > 0)
                    {
                        _announced = mark;
                        var text = $"The server {Verb} in {Span(mark)}.";
                        host.Broadcast(mark >= 60 ? Line(text) : text);  // the message with the longer notices only
                    }
                    return;
                }
                host.Broadcast($"The server {Verb} now: saving the world.");
                State = RestartState.Saving;
                _saving = host.SaveAndMark(Marker());
                return;
            }
            case RestartState.Saving:
            {
                if (_saving is not { IsCompleted: true } t) return;
                var why = t.IsCompletedSuccessfully ? t.Result : t.Exception?.GetBaseException().Message ?? "canceled";
                if (why is not null)
                {
                    Fail($"not saved, so not stopped: {why}", markerWritten: false);
                    return;
                }
                State = RestartState.Stopping;
                NextAttempt();
                return;
            }
            case RestartState.Stopping:
                if (host.NowMs < _deadline) return;
                host.Log(LogLevel.Warn, $"restart: the stop raised through {_how} did not begin within {StopWaitMs / 1000} s");
                NextAttempt();
                return;
        }
    }

    private void NextAttempt()
    {
        while (_attempt < 2)
        {
            _attempt++;
            string? how;
            try
            {
                how = host.RaiseStop(_attempt);
            }
            catch (Exception e)
            {
                host.Log(LogLevel.Warn, $"restart: raising the stop (way {_attempt}) failed: {e.Message}");
                how = null;
            }
            if (how is null) continue;
            _how = how;
            _deadline = host.NowMs + StopWaitMs;
            host.Log(LogLevel.Info, $"restart: the world is saved; stopping the server through {how} (as a Ctrl-C does)");
            return;
        }
        Fail(_how is null ? "this server offers no way to raise its stop (CtrlRoutine, GenerateConsoleCtrlEvent)"
                          : "the server did not begin to stop", markerWritten: true);
    }

    private void Fail(string why, bool markerWritten)
    {
        State = RestartState.Failed;
        _error = why;
        if (markerWritten) host.DeleteMarker();
        host.Log(LogLevel.Error, $"restart failed: {why}. The server keeps running; stop it from its host.");
        host.Broadcast($"The server {(support.Supported ? "restart" : "stop")} failed; the server keeps running.");
    }

    private JsonObject Marker() => new()
    {
        ["format"] = 1, ["runId"] = support.RunId, ["pid"] = Environment.ProcessId, ["at"] = _requested, ["by"] = _by,
        ["message"] = _message.Length > 0 ? _message : null,
    };

    /// <summary>For admin_info and the op's answer: null before any request.</summary>
    public JsonObject? Info()
    {
        if (State is not { } st) return null;
        var left = Math.Max(0, _at - host.NowMs);
        return new JsonObject
        {
            ["state"] = st.ToString().ToLowerInvariant(), ["by"] = _by, ["requested"] = _requested,
            ["secondsLeft"] = st == RestartState.Counting ? (int)((left + 999) / 1000) : 0,
            ["message"] = _message.Length > 0 ? _message : null, ["error"] = _error, ["via"] = _how,
            ["restart"] = support.ToJson(),
        };
    }

    private string Describe() => State switch
    {
        RestartState.Counting => $"counting down ({Math.Max(0, (_at - host.NowMs + 999) / 1000)} s left, asked by {_by})",
        RestartState.Saving => "saving the world",
        RestartState.Stopping => "stopping the server",
        _ => "done",
    };

    private string Line(string text) => _message.Length > 0 ? $"{text} {_message}" : text;

    internal static string Span(int seconds) => seconds switch
    {
        >= 120 when seconds % 60 == 0 => $"{seconds / 60} minutes",
        60 => "1 minute",
        1 => "1 second",
        _ => $"{seconds} seconds",
    };
}

/// <summary>The server's own stop, raised from inside: the console control routine a Ctrl-C runs (Wine runs it on
/// SIGINT too), so Kiln.Host's handler unloads the mods and the game saves and exits as with `kilnctl stop`.</summary>
internal static partial class ConsoleStop
{
    private const uint CtrlCEvent = 0;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandleW(string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true, BestFitMapping = false)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateThread(IntPtr attributes, UIntPtr stackSize, IntPtr start, IntPtr parameter, uint flags, out uint id);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroupId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetConsoleCtrlHandler(IntPtr handler, [MarshalAs(UnmanagedType.Bool)] bool add);

    /// <summary>Attempt 1: <c>CtrlRoutine(CTRL_C_EVENT)</c> (kernelbase, else kernel32) on a thread of its own. Attempt
    /// 2: <c>GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)</c> (needs a console). Returns how, or null when that way is not
    /// there (not Windows, no such export, no console).</summary>
    public static string? Raise(int attempt)
    {
        if (!OperatingSystem.IsWindows()) return null;
        // a process started in a new process group ignores Ctrl-C (CtrlRoutine then calls no handler): turn that off
        SetConsoleCtrlHandler(IntPtr.Zero, false);
        if (attempt == 1)
        {
            foreach (var dll in new[] { "kernelbase.dll", "kernel32.dll" })
            {
                var module = GetModuleHandleW(dll);
                if (module == IntPtr.Zero) continue;
                var routine = GetProcAddress(module, "CtrlRoutine");
                if (routine == IntPtr.Zero) continue;
                var thread = CreateThread(IntPtr.Zero, UIntPtr.Zero, routine, (IntPtr)CtrlCEvent, 0, out _);
                if (thread == IntPtr.Zero) continue;
                CloseHandle(thread);
                return dll + "!CtrlRoutine";
            }
            return null;
        }
        return attempt == 2 && GenerateConsoleCtrlEvent(CtrlCEvent, 0) ? "GenerateConsoleCtrlEvent" : null;
    }
}

internal static partial class Ops
{
    /// <summary>Set by AdminMod: the restart under way (or the last one), driven from Pre.</summary>
    internal static RestartController? Restart { get; set; }

    internal const string RestartMarker = "restart.json";

    internal static void RegisterRestart(Bridge bridge, Logger log, RestartController restart)
    {
        Restart = restart;
        // {seconds?: 0..3600 (default 60), message?: <= 200 characters, by?: <= 120 characters}
        bridge.Register("server_restart", req =>
        {
            var seconds = RestartController.DefaultSeconds;
            if (req["seconds"] is not null)
            {
                var n = Number(req, "seconds");
                if (n < 0 || n > RestartController.MaxSeconds || n != Math.Floor(n)) throw new AdminError($"seconds: a whole number, 0 to {RestartController.MaxSeconds}");
                seconds = (int)n;
            }
            var message = OptionalText(req, "message", RestartController.MaxMessage);
            return restart.Schedule(seconds, message, By(req));
        });
        bridge.Register("server_restart_cancel", req => restart.Cancel(By(req)));
    }

    /// <summary>Who asked: the request's <c>by</c>, else the KAP client's name, else "file bridge".</summary>
    internal static string By(JsonObject req) =>
        OptionalText(req, "by", RestartController.MaxBy) ?? Tcp?.CurrentCallerName ?? "file bridge";

    internal static string? OptionalText(JsonObject req, string key, int max)
    {
        if (req[key] is null) return null;
        var text = req[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s.Trim() : throw new AdminError($"{key} must be text");
        if (text.Length > max) throw new AdminError($"{key} is longer than {max} characters");
        if (text.Any(char.IsControl)) throw new AdminError($"{key} must be one line");
        return text.Length == 0 ? null : text;
    }
}

/// <summary>The real server: Kiln.Chat for the notices, Kiln.Saves for the save, the marker in Kiln.Admin's data folder.</summary>
internal sealed class GameRestartHost(Logger log, string dataDirectory) : IRestartHost
{
    private readonly object _markLock = new();
    private bool _abandoned, _marked;

    public long NowMs => Environment.TickCount64;

    public string MarkerPath => Path.Combine(dataDirectory, Ops.RestartMarker);

    public void Broadcast(string text)
    {
        try
        {
            if (Chat.Available) Chat.Broadcast(text);
        }
        catch (Exception e)
        {
            log.Warn($"restart notice not sent: {e.Message}");
        }
    }

    public async Task<string?> SaveAndMark(JsonObject marker)
    {
        if (Saves.Available)
        {
            var r = await Saves.Request();
            if (!r.Success) return r.Error ?? "not saved";
        }
        else
        {
            log.Info("restart: Kiln.Saves is not available here; the game's own save on the way out is the only one");
        }
        var text = marker.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
        return await Task.Run(() =>
        {
            lock (_markLock)
            {
                if (_abandoned) return "Kiln.Admin unloaded";
                try
                {
                    Directory.CreateDirectory(dataDirectory);
                    Bridge.WriteAtomic(MarkerPath, text);
                    _marked = true;
                    return (string?)null;
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    return $"cannot write {MarkerPath}: {e.Message}";
                }
            }
        });
    }

    /// <summary>OnUnload of a restart that never raised its stop (a hot reload of Kiln.Admin while it counted down or
    /// saved): no marker may stay, or a later normal stop would come back as a restart.</summary>
    public void Abandon()
    {
        lock (_markLock)
        {
            _abandoned = true;
            if (_marked) DeleteMarker(MarkerPath, log);
        }
    }

    public void DeleteMarker() => Task.Run(() => DeleteMarker(MarkerPath, log));

    /// <summary>Removes a marker (a failed stop; a leftover at load, which kilnctl did not take).</summary>
    public static bool DeleteMarker(string path, Logger log)
    {
        try
        {
            if (!File.Exists(path)) return false;
            File.Delete(path);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            log.Warn($"cannot delete {path}: {e.Message}");
            return false;
        }
    }

    public string? RaiseStop(int attempt) => ConsoleStop.Raise(attempt);

    public void Log(LogLevel level, string text) => log.Write(level, text);
}
