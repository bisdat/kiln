// SPDX-License-Identifier: MIT
// What the Kiln menu's Network page shows of the network inspector (Kiln.Ui's "Replication" section): the counters as
// text, refreshed on the tick thread while the menu draws them, and Kiln's own hold on the counters, which the section's
// switch turns on and off. The menu draws on the render thread and never calls kiln.repl itself.
using System.Collections.Concurrent;
using System.Globalization;

namespace Kiln;

internal static class ReplicationView
{
    private static volatile IReadOnlyList<string> _lines = [];
    private static long _wantedUntilMs;  // Environment.TickCount64: the menu drew the section until about then
    private static readonly ConcurrentQueue<bool> Switches = new();
    private static IDisposable? _tick;
    private static int _step;

    /// <summary>kiln.repl can count what this process sends (a server, or a hosted world's own client).</summary>
    public static bool Available => (ReplNative.Capabilities() & ReplNative.CapObserve) != 0;

    /// <summary>Kiln's own counters are on (the section's switch).</summary>
    public static bool CountingOn => Replication.StatsOn(Replication.ServiceOwner);

    /// <summary>The section's switch (render thread): applied on the tick thread at its next step.</summary>
    public static void SetCounting(bool on) => Switches.Enqueue(on);

    /// <summary>The counters as lines (render thread); asking keeps them fresh for a second.</summary>
    public static IReadOnlyList<string> Lines
    {
        get
        {
            Volatile.Write(ref _wantedUntilMs, Environment.TickCount64 + 1000);
            return _lines;
        }
    }

    /// <summary>At the service's start: a service tick that applies the switch and refreshes the lines.</summary>
    internal static void Start()
    {
        if (_tick != null || !Available) return;
        _tick = ServiceTick.Add("kiln replication view", static (in TickInfo _) => Step());
    }

    private static void Step()
    {
        try
        {
            while (Switches.TryDequeue(out var on))
            {
                if (on) Replication.StatsFor(Replication.ServiceOwner);
                else Replication.StopStats(Replication.ServiceOwner);
                _lines = [];
            }
            if (Environment.TickCount64 > Volatile.Read(ref _wantedUntilMs) || ++_step % 30 != 0) return;
            if (!CountingOn)
            {
                _lines = [];
                return;
            }
            var rows = Replication.StatsFor(Replication.ServiceOwner);
            var lines = new List<string>(rows.Count + 1);
            foreach (var r in rows) lines.Add(ReplCommands.Line(r));
            if (rows.Count == 0) lines.Add("nothing sent since the counters went on");
            lines.Add(string.Create(CultureInfo.InvariantCulture, $"updated {DateTime.Now:HH:mm:ss}"));
            _lines = lines;
        }
        catch (Exception)
        {
            _lines = ["the counters are not available here (see kiln.log)"];
        }
    }
}
