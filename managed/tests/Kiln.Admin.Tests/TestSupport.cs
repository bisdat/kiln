// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace Kiln.Admin.Tests;

/// <summary>Kiln's log as a list (Backend.Current is process-wide: the tests run one at a time).</summary>
internal sealed class CaptureBackend : IKilnBackend
{
    public readonly List<string> Lines = [];
    public string Version => "test";
    public bool IsTickThread => true;
    public bool HasFeature(string name) => false;
    public nint Service(string name, uint minVersion) => 0;

    public void Log(ulong owner, LogLevel level, string text)
    {
        lock (Lines) Lines.Add($"[{level}] {text}");
    }

    public bool Logged(string part)
    {
        lock (Lines) return Lines.Any(l => l.Contains(part, StringComparison.Ordinal));
    }

    public string All()
    {
        lock (Lines) return string.Join("\n", Lines);
    }
}

/// <summary>A temp folder, a logger that writes into <see cref="Backend"/>.</summary>
public abstract class AdminTest : IDisposable
{
    private readonly IKilnBackend _previous = Kiln.Backend.Current;
    internal readonly CaptureBackend Backend = new();
    internal readonly Logger Log = new(1);
    protected readonly string Dir = Path.Combine(Path.GetTempPath(), "kiln-admin-tests-" + Guid.NewGuid().ToString("N"));

    protected AdminTest()
    {
        Kiln.Backend.Current = Backend;
        Directory.CreateDirectory(Dir);
    }

    public virtual void Dispose()
    {
        Kiln.Backend.Current = _previous;
        try
        {
            Directory.Delete(Dir, true);
        }
        catch (Exception)
        {
        }
    }

    /// <summary>Waits up to <paramref name="ms"/> for <paramref name="condition"/>.</summary>
    protected static bool Eventually(Func<bool> condition, int ms = 3000)
    {
        var until = Environment.TickCount64 + ms;
        while (Environment.TickCount64 < until)
        {
            if (condition()) return true;
            Thread.Sleep(10);
        }
        return condition();
    }
}

/// <summary>A named pipe stands in for a disk that does not answer: open() of a FIFO blocks until the other end opens.</summary>
internal static class Fifo
{
    [DllImport("libc", SetLastError = true)]
    private static extern int mkfifo(string path, uint mode);

    public static void Make(string path)
    {
        if (mkfifo(path, Convert.ToUInt32("600", 8)) != 0) throw new IOException($"mkfifo {path}: errno {Marshal.GetLastPInvokeError()}");
    }

    /// <summary>Opens the other end (read or write) on a worker so a blocked open() on this side goes on; returns what
    /// was read (read side) or writes <paramref name="text"/> (write side).</summary>
    public static Task<string> Release(string path, bool read, string text = "") => Task.Run(() =>
    {
        if (read)
        {
            using var r = new FileStream(path, FileMode.Open, FileAccess.Read);
            using var sr = new StreamReader(r);
            return sr.ReadToEnd();
        }
        using var w = new FileStream(path, FileMode.Open, FileAccess.Write);
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        w.Write(bytes);
        return "";
    });
}
