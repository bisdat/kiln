// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Text.Json;
using Microsoft.Build.Framework;

namespace Kiln.Sdk;

/// <summary>
/// Reads a server's <c>kiln/state/debug/session.json</c> (written by <c>kiln debug --dap</c>, kiln_manage::debug::SessionFile)
/// for KilnDeploy's reload guard (KILN012): <see cref="Attached"/> is true only when the file says
/// attaching/running/paused/detaching AND the proxy that wrote it still runs (its PID, started at its <c>proxyStarted</c>
/// Unix second, as <c>kiln reload</c> checks). A file left by a proxy that ended gives a warning and no refusal.
/// </summary>
public sealed class KilnDebugSession : Microsoft.Build.Utilities.Task
{
    [Required] public string File { get; set; } = "";

    /// <summary>A live debug session holds this server.</summary>
    [Output] public bool Attached { get; set; }

    /// <summary>Who holds it, for the error text ("kiln debug, pid N, paused").</summary>
    [Output] public string Holder { get; set; } = "";

    public override bool Execute()
    {
        string text;
        try
        {
            text = System.IO.File.ReadAllText(File);
        }
        catch (IOException)
        {
            return true;  // removed meanwhile
        }
        var (state, pid, attached) = Check(text, StartOf);
        Attached = attached;
        Holder = $"kiln debug, pid {pid}, {state}";
        if (state is not null && !attached && IsAttachedState(state))
            Log.LogWarning(null, "KILN012", null, File, 0, 0, 0, 0,
                $"{File} says a debugger is attached ({state}), but the kiln debug that wrote it (pid {pid}) is gone: the reload goes ahead. Delete the file if this repeats.");
        return true;
    }

    private static bool IsAttachedState(string s) => s is "attaching" or "running" or "paused" or "detaching";

    /// <summary>The session's state and proxy PID, and whether it holds the server: an attached state written by a proxy
    /// that still runs (<paramref name="startOf"/> gives a PID's start as Unix seconds, null when no such process).</summary>
    public static (string? State, int Pid, bool Attached) Check(string json, Func<int, long?> startOf)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            var state = root.TryGetProperty("state", out var s) && s.TryGetProperty("state", out var st) ? st.GetString() : null;
            var pid = root.TryGetProperty("proxyPid", out var p) && p.TryGetInt32(out var n) ? n : 0;
            if (state is null || !IsAttachedState(state) || pid <= 0) return (state, pid, false);
            var started = startOf(pid);
            if (started is null) return (state, pid, false);
            // proxyStarted null: the writer could not tell its start; the PID alone has to do
            if (root.TryGetProperty("proxyStarted", out var ps) && ps.ValueKind == JsonValueKind.Number && Math.Abs(ps.GetInt64() - started.Value) > 2)
                return (state, pid, false);
            return (state, pid, true);
        }
        catch (JsonException)
        {
            return (null, 0, false);
        }
    }

    private static long? StartOf(int pid)
    {
        try
        {
            using var p = Process.GetProcessById(pid);
            return new DateTimeOffset(p.StartTime.ToUniversalTime()).ToUnixTimeSeconds();
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        {
            return null;
        }
    }
}
