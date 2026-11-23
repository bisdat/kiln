// SPDX-License-Identifier: MIT
using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace Kiln.Admin;

/// <summary>The op table and its two transports' shared entry point, <see cref="Execute"/>.
/// <para>The file bridge for external admin tools: the tool writes
/// <c>requests/&lt;id&gt;.json</c> ({"op": ...}); each request runs once on the tick thread, is deleted, and
/// <c>results/&lt;id&gt;.json</c> gets {ok, op, ms, data | error}; <c>state.json</c> is refreshed about once a second.
/// Writes go through a temp file + rename so the dashboard never reads half a file.</para>
/// <para>KAP/1 (<see cref="TcpAdmin"/>) calls <see cref="Execute"/> with the same request objects, so every op answers
/// the same over both transports (docs/kiln/launcher/admin-protocol.md).</para>
/// <para>The disk is only touched by the bridge's own I/O thread (started at the first use): it reads and deletes the
/// request files, writes the result files and state.json. The tick thread hands texts over and takes parsed requests,
/// never waiting for the disk (step 0 of the web admin saw one Pre handler take 810 ms while Kiln.Saves waited 3.66 s
/// for the disk). When the disk is slow, state.json gets the newest state once it answers; states in between are
/// skipped.</para></summary>
internal sealed partial class Bridge(string directory, Logger log) : IDisposable
{
    public delegate JsonNode? Op(JsonObject request);
    public delegate Task<JsonNode?> AsyncOp(JsonObject request);

    private sealed record Pending(string Name, Task<JsonNode?> Task, long Started, Action<JsonObject> Done);
    private readonly List<Pending> _pending = [];
    private readonly Dictionary<string, AsyncOp> _async = new(StringComparer.Ordinal);

    private static readonly JsonSerializerOptions Json = new() { WriteIndented = false };
    private readonly Dictionary<string, Op> _ops = new(StringComparer.Ordinal);
    private readonly List<string> _order = [];
    private readonly string _requests = Path.Combine(directory, "requests");
    private readonly string _results = Path.Combine(directory, "results");

    public string Directory => directory;
    public IReadOnlyCollection<string> Ops => _ops.Keys;

    /// <summary>Op names in registration order (stable across transports and runs).</summary>
    public IReadOnlyList<string> OpNames => _order;

    public bool Has(string name) => _ops.ContainsKey(name);

    /// <summary>Ops still running (async ops awaiting their task).</summary>
    public int PendingCount => _pending.Count;

    [GeneratedRegex("^[A-Za-z0-9-]{1,64}\\.json$")]
    private static partial Regex RequestName();

    // ---- the I/O thread
    private const int ScanMs = 80;         // the requests folder, as often as the tick thread used to poll it
    private const int MaxIncoming = 64;    // parsed requests waiting for the tick thread; the scan pauses above this
    private const int SlowWriteMs = 1000;  // a write this slow gets one log line a minute
    private readonly ConcurrentQueue<Incoming> _incoming = new();
    private readonly ConcurrentQueue<(string Path, string Text)> _out = new();  // result files to write
    private readonly AutoResetEvent _wake = new(false);
    private readonly object _ioLock = new();
    private Thread? _io;
    private volatile bool _ioStop;
    private string? _state;  // the newest state.json text not yet written (Interlocked)
    private long _lastSlowLog, _lastIoWarn;

    private sealed record Incoming(string ResultPath, JsonObject? Request, string? Error);

    /// <summary>How a file is written (path, text): <see cref="WriteAtomic"/>; tests make the disk slow here.</summary>
    internal Action<string, string> WriteFile { get; set; } = WriteAtomic;

    /// <summary>state.json texts that were replaced by a newer one before the disk took them.</summary>
    internal long StatesSkipped;

    public void Register(string name, Op op)
    {
        if (!_ops.ContainsKey(name)) _order.Add(name);
        _ops[name] = op;
    }

    /// <summary>An op that completes later on the tick thread (e.g. a teleport); its result is delivered when done.</summary>
    public void RegisterAsync(string name, AsyncOp op)
    {
        _async[name] = op;
        Register(name, _ => throw new InvalidOperationException("async op called synchronously"));
    }

    public void EnsureDirectories()
    {
        System.IO.Directory.CreateDirectory(_requests);
        System.IO.Directory.CreateDirectory(_results);
    }

    /// <summary>Seconds since something else wrote this folder's state.json: a file newer than
    /// <paramref name="fresh"/> that is not Kiln.Admin's (its state always has "kiln"). Null when there is none. Asked
    /// before the first write of a load, so a state.json found then is not this instance's.</summary>
    public double? ForeignState(double fresh = 5)
    {
        try
        {
            var file = new FileInfo(Path.Combine(directory, "state.json"));
            if (!file.Exists) return null;
            var age = (DateTime.UtcNow - file.LastWriteTimeUtc).TotalSeconds;
            if (age > fresh) return null;
            return JsonNode.Parse(File.ReadAllText(file.FullName)) is JsonObject o && o.ContainsKey("kiln") ? null : Math.Max(0, age);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            return null;
        }
    }

    /// <summary>File bridge, tick thread: runs up to <paramref name="max"/> requests the I/O thread has read (oldest
    /// first); their results go back to it to be written. No disk access here.</summary>
    public int Poll(int max = 16)
    {
        FinishPending();
        EnsureIo();
        var n = 0;
        while (n < max && _incoming.TryDequeue(out var r))
        {
            n++;
            var path = r.ResultPath;
            if (r.Request is null)
            {
                QueueResult(path, (r.Error is null ? new JsonObject { ["ok"] = false } : new JsonObject { ["ok"] = false, ["error"] = r.Error }).ToJsonString(Json));
                continue;
            }
            Execute(r.Request, result => QueueResult(path, result.ToJsonString(Json)));
        }
        if (n == max) _wake.Set();  // more may be waiting
        return n;
    }

    private void QueueResult(string path, string text)
    {
        _out.Enqueue((path, text));
        _wake.Set();
    }

    /// <summary>Starts the I/O thread (once; again after <see cref="Dispose"/> is not supported).</summary>
    private void EnsureIo()
    {
        if (_io is not null || _ioStop) return;
        lock (_ioLock)
        {
            if (_io is not null || _ioStop) return;
            var t = new Thread(IoLoop) { IsBackground = true, Name = "kiln-admin bridge io" };
            t.Start();
            _io = t;
        }
    }

    private void IoLoop()
    {
        while (true)
        {
            _wake.WaitOne(ScanMs);
            var stopping = _ioStop;
            try
            {
                if (stopping) AnswerUnrun();
                WriteOut();
                if (stopping) return;
                // a stop that came while this thread waited for the disk: take no more requests (their files would be
                // deleted, and nothing would run them)
                if (!_ioStop && _incoming.Count < MaxIncoming) Scan();
            }
            catch (Exception e)
            {
                if (stopping) return;
                if (Environment.TickCount64 - Interlocked.Read(ref _lastIoWarn) > 60_000)
                {
                    Interlocked.Exchange(ref _lastIoWarn, Environment.TickCount64);
                    log.Warn($"file bridge I/O: {e.Message}");
                }
            }
        }
    }

    /// <summary>I/O thread, at the stop: requests it read (their files are deleted) that the tick thread never ran get a
    /// failed result, so no caller waits for an answer that cannot come. Before the I/O thread such a request stayed in
    /// the folder for the next Kiln.Admin; a caller now sends it again.</summary>
    private void AnswerUnrun()
    {
        while (_incoming.TryDequeue(out var r))
        {
            var op = r.Request?["op"] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;
            _out.Enqueue((r.ResultPath, new JsonObject
            {
                ["op"] = op, ["ok"] = false, ["error"] = "Kiln.Admin unloaded before it ran this request; send it again",
            }.ToJsonString(Json)));
        }
    }

    /// <summary>I/O thread: the results waiting, then the newest state.</summary>
    private void WriteOut()
    {
        while (_out.TryDequeue(out var r)) Write(r.Path, r.Text);
        if (Interlocked.Exchange(ref _state, null) is { } state) Write(Path.Combine(directory, "state.json"), state);
    }

    private void Write(string path, string text)
    {
        var started = Environment.TickCount64;
        try
        {
            WriteFile(path, text);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            log.Warn($"file bridge: cannot write {path}: {e.Message}");
        }
        var ms = Environment.TickCount64 - started;
        if (ms >= SlowWriteMs && Environment.TickCount64 - Interlocked.Read(ref _lastSlowLog) > 60_000)
        {
            Interlocked.Exchange(ref _lastSlowLog, Environment.TickCount64);
            log.Info($"file bridge: writing {Path.GetFileName(path)} took {ms} ms (the disk is busy); the server's steps did not wait for it");
        }
    }

    /// <summary>I/O thread: reads (and deletes) up to 16 request files, oldest first.</summary>
    private void Scan()
    {
        if (!System.IO.Directory.Exists(_requests)) return;
        var files = new DirectoryInfo(_requests).EnumerateFiles("*.json")
            .Where(f => RequestName().IsMatch(f.Name))
            .OrderBy(f => f.LastWriteTimeUtc).Take(16).ToList();
        foreach (var f in files) Read(f.FullName);
    }

    private void Read(string file)
    {
        var id = Path.GetFileNameWithoutExtension(file);
        string raw;
        try
        {
            raw = File.ReadAllText(file);
            File.Delete(file);
        }
        catch (IOException)
        {
            return;  // still being written; next scan
        }
        var resultPath = Path.Combine(_results, id + ".json");
        try
        {
            _incoming.Enqueue(new Incoming(resultPath, JsonNode.Parse(raw) as JsonObject, null));
        }
        catch (JsonException e)
        {
            _incoming.Enqueue(new Incoming(resultPath, null, "bad request: " + e.Message));
        }
    }

    /// <summary>Runs one request ({"op": name, ...args}) on the tick thread. <paramref name="done"/> gets the result
    /// object {op, data, ok, ms} | {op, ok, error, ms} (unknown op: {op, ok, error}): at once for a synchronous op,
    /// from a later <see cref="Poll"/>/<see cref="FinishPending"/> for an async one. Key order is the file bridge's
    /// (the file format is stable, byte for byte).</summary>
    public void Execute(JsonObject request, Action<JsonObject> done)
    {
        var name = request["op"] is JsonValue v && v.TryGetValue<string>(out var s) ? s : "";
        var result = new JsonObject { ["op"] = name };
        if (!_ops.TryGetValue(name, out var op))
        {
            result["ok"] = false;
            result["error"] = "unknown op " + name;
            Deliver(done, result, name);
            return;
        }
        if (_async.TryGetValue(name, out var asyncOp))
        {
            Task<JsonNode?> task;
            try
            {
                task = asyncOp(request);
            }
            catch (Exception e)
            {
                task = Task.FromException<JsonNode?>(e);
            }
            _pending.Add(new Pending(name, task, System.Diagnostics.Stopwatch.GetTimestamp(), done));
            FinishPending();
            return;
        }
        var started = System.Diagnostics.Stopwatch.GetTimestamp();
        try
        {
            result["data"] = op(request);
            result["ok"] = true;
        }
        catch (AdminError e)
        {
            result["ok"] = false;
            result["error"] = e.Message;
        }
        catch (Exception e)
        {
            result["ok"] = false;
            result["error"] = e.Message;
            log.Error(e, $"op {name}");
        }
        result["ms"] = (int)System.Diagnostics.Stopwatch.GetElapsedTime(started).TotalMilliseconds;
        if (name != "ping") log.Info($"{name} {(result["ok"]!.GetValue<bool>() ? "ok" : "failed: " + result["error"])}");
        Deliver(done, result, name);
    }

    /// <summary>Delivers finished async ops (called every poll; KAP's drain calls it too).</summary>
    public void FinishPending()
    {
        for (var i = _pending.Count - 1; i >= 0; i--)
        {
            var p = _pending[i];
            if (!p.Task.IsCompleted) continue;
            _pending.RemoveAt(i);
            var result = new JsonObject { ["op"] = p.Name };
            if (p.Task.IsCompletedSuccessfully)
            {
                result["data"] = p.Task.Result;
                result["ok"] = true;
            }
            else
            {
                var e = p.Task.Exception?.GetBaseException();
                result["ok"] = false;
                result["error"] = e?.Message ?? "cancelled";
                if (e is not AdminError and not null) log.Error(e, $"op {p.Name}");
            }
            result["ms"] = (int)System.Diagnostics.Stopwatch.GetElapsedTime(p.Started).TotalMilliseconds;
            log.Info($"{p.Name} {(result["ok"]!.GetValue<bool>() ? "ok" : "failed: " + result["error"])}");
            Deliver(p.Done, result, p.Name);
        }
    }

    /// <summary>Unload: async ops that never finished are answered as failed (KAP clients get shutting_down from the
    /// listener anyway; file-bridge results are simply not written, as before).</summary>
    public void Abandon() => _pending.Clear();

    /// <summary>Unload: the I/O thread writes what it has (results, the last state) and ends. It waits for the disk at
    /// most <paramref name="waitMs"/> ms; a thread still stuck in a write then ends on its own after it.</summary>
    public void Dispose() => StopIo(1000);

    internal bool StopIo(int waitMs)
    {
        Thread? t;
        lock (_ioLock)
        {
            _ioStop = true;
            t = _io;
        }
        if (t is null) return true;
        _wake.Set();
        if (t.Join(waitMs)) return true;
        log.Warn($"file bridge: the I/O thread is still waiting for the disk after {waitMs} ms; it ends after that write");
        return false;
    }

    private void Deliver(Action<JsonObject> done, JsonObject result, string name)
    {
        try
        {
            done(result);
        }
        catch (Exception e)
        {
            log.Error(e, $"delivering the result of {name}");
        }
    }

    /// <summary>The state.json text (also pushed to KAP subscribers).</summary>
    public static string Serialize(JsonNode node) => node.ToJsonString(Json);

    public void WriteState(JsonObject state) => WriteStateText(Serialize(state));

    /// <summary>Tick thread: hands state.json's new text to the I/O thread (never waits for the disk; a text not yet
    /// written is replaced by this one).</summary>
    public void WriteStateText(string text)
    {
        EnsureIo();
        if (Interlocked.Exchange(ref _state, text) is not null) Interlocked.Increment(ref StatesSkipped);
        _wake.Set();
    }

    internal static void WriteAtomic(string path, string text)
    {
        var tmp = Path.Combine(Path.GetDirectoryName(path)!, "." + Path.GetFileName(path) + ".tmp");
        File.WriteAllText(tmp, text);
        File.Move(tmp, path, overwrite: true);
    }
}

/// <summary>An expected failure: its message goes to the dashboard as-is, no stack trace in the log.</summary>
internal sealed class AdminError(string message) : Exception(message);
