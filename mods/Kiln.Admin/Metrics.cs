// SPDX-License-Identifier: MIT
// The KAP topic "metrics" (docs/kiln/launcher/admin-protocol.md section 5): every 2 s, to its subscribers only, the frame
// time (from one step's start to the next), the tick time (one step itself: Pre to Post), process memory, CPU and the
// player count. Nobody subscribed: the sampler does nothing and Kiln.Admin has no Post handler (a Post listener costs
// the server some parallelism). The tick thread only records two numbers per step into fixed arrays; the process
// figures and the JSON are made on KAP's housekeeping thread.
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace Kiln.Admin;

/// <summary>avg, p95 and max of one window, in milliseconds; <see cref="Count"/> samples.</summary>
internal readonly record struct StepStat(double Avg, double P95, double Max, int Count)
{
    public static readonly StepStat None = new(0, 0, 0, 0);
}

/// <summary>One finished 2 s window, as the tick thread saw it.</summary>
internal sealed record MetricsWindow(long AtUnixMs, double Seconds, StepStat Frame, StepStat Tick, int Players);

/// <summary>Tick thread only. <see cref="Pre"/> every step, <see cref="Post"/> while there is a subscriber.</summary>
internal sealed class MetricsSampler(int windowMs = MetricsSampler.WindowMs)
{
    public const int WindowMs = 2000;
    private const int Capacity = 1024;  // 2 s at 60 steps/s is 120; more steps only count in avg and max
    private readonly double[] _frames = new double[Capacity], _ticks = new double[Capacity], _scratch = new double[Capacity];
    private readonly Acc _f = new(), _t = new();
    private bool _running;
    private long _windowStart, _preTs;

    private sealed class Acc
    {
        public int N;
        public long Count;
        public double Sum, Max;
        public void Reset() => (N, Count, Sum, Max) = (0, 0, 0, 0);
    }

    /// <summary>Recording (a subscriber exists).</summary>
    public bool Running => _running;

    /// <summary>The start of a step. <paramref name="active"/> false: nothing is recorded (and a running window is
    /// dropped). Returns the window that just ended, every <see cref="WindowMs"/>; null otherwise.
    /// <paramref name="players"/> is only called then.</summary>
    public MetricsWindow? Pre(bool active, double wallDeltaMs, long now, Func<int> players)
    {
        if (!active)
        {
            if (_running) Reset();
            return null;
        }
        if (!_running)
        {
            _running = true;
            _windowStart = _preTs = now;
            return null;  // the interval up to this step started before anyone listened
        }
        Add(_frames, _f, wallDeltaMs);
        _preTs = now;
        var elapsed = Stopwatch.GetElapsedTime(_windowStart, now).TotalMilliseconds;
        if (elapsed < windowMs) return null;
        var w = new MetricsWindow(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), elapsed / 1000.0, Stat(_frames, _f), Stat(_ticks, _t), SafeCount(players));
        _f.Reset();
        _t.Reset();
        _windowStart = now;
        return w;
    }

    /// <summary>The end of a step (Context.Tick.Post): Pre to here is the step itself.</summary>
    public void Post(long now)
    {
        if (!_running) return;
        Add(_ticks, _t, Stopwatch.GetElapsedTime(_preTs, now).TotalMilliseconds);
    }

    private void Reset()
    {
        _running = false;
        _f.Reset();
        _t.Reset();
    }

    private static void Add(double[] samples, Acc a, double ms)
    {
        if (!double.IsFinite(ms) || ms < 0) return;
        if (a.N < samples.Length) samples[a.N++] = ms;
        a.Count++;
        a.Sum += ms;
        if (ms > a.Max) a.Max = ms;
    }

    private StepStat Stat(double[] samples, Acc a)
    {
        if (a.Count == 0) return StepStat.None;
        Array.Copy(samples, _scratch, a.N);
        Array.Sort(_scratch, 0, a.N);
        var p95 = _scratch[Math.Clamp((int)Math.Ceiling(a.N * 0.95) - 1, 0, a.N - 1)];
        return new StepStat(a.Sum / a.Count, p95, a.Max, (int)Math.Min(a.Count, int.MaxValue));
    }

    private static int SafeCount(Func<int> players)
    {
        try
        {
            return players();
        }
        catch (Exception)
        {
            return -1;
        }
    }
}

/// <summary>The process's figures, read off the tick thread (they read /proc under Wine).</summary>
internal sealed class ProcessProbe
{
    private TimeSpan _cpu;
    private long _at;
    private TimeSpan _gcPause = GC.GetTotalPauseDuration();

    public readonly record struct Sample(double? CpuPercent, long WorkingSet, long ManagedHeap, double GcPauseMs);

    /// <summary>CPU since the previous call (100 = one core busy; null on the first call or when unreadable), working
    /// set, the managed heap (Kiln and every mod share it), GC pause time since the previous call.</summary>
    public Sample Read()
    {
        double? cpu = null;
        long ws = 0;
        try
        {
            using var p = Process.GetCurrentProcess();
            var total = p.TotalProcessorTime;
            ws = p.WorkingSet64;
            var now = Stopwatch.GetTimestamp();
            if (_at != 0)
            {
                var wall = Stopwatch.GetElapsedTime(_at, now).TotalMilliseconds;
                if (wall > 0) cpu = Math.Max(0, (total - _cpu).TotalMilliseconds / wall * 100.0);
            }
            _cpu = total;
            _at = now;
        }
        catch (Exception)
        {
        }
        var pause = GC.GetTotalPauseDuration();
        var gc = (pause - _gcPause).TotalMilliseconds;
        _gcPause = pause;
        return new Sample(cpu, ws, GC.GetTotalMemory(false), gc);
    }
}

internal static class MetricsJson
{
    /// <summary>The push's data object:
    /// <c>{at, windowS, steps, stepsPerS, frameMs {avg, p95, max}, tickMs {avg, p95, max} | null, memory {workingSet,
    /// managedHeap}, cpu {percent | null, cores}, gcPauseMs, players | null}</c>. Written by hand: one string per push.</summary>
    public static string Data(MetricsWindow w, ProcessProbe.Sample p)
    {
        var sb = new StringBuilder(384);
        sb.Append("{\"at\":").Append(w.AtUnixMs)
          .Append(",\"windowS\":").Append(N(w.Seconds, 3))
          .Append(",\"steps\":").Append(w.Frame.Count)
          .Append(",\"stepsPerS\":").Append(N(w.Seconds > 0 ? w.Frame.Count / w.Seconds : 0, 2))
          .Append(",\"frameMs\":");
        Stat(sb, w.Frame);
        sb.Append(",\"tickMs\":");
        if (w.Tick.Count == 0) sb.Append("null");
        else Stat(sb, w.Tick);
        sb.Append(",\"memory\":{\"workingSet\":").Append(p.WorkingSet).Append(",\"managedHeap\":").Append(p.ManagedHeap).Append('}')
          .Append(",\"cpu\":{\"percent\":").Append(p.CpuPercent is { } c ? N(c, 1) : "null").Append(",\"cores\":").Append(Environment.ProcessorCount).Append('}')
          .Append(",\"gcPauseMs\":").Append(N(p.GcPauseMs, 1))
          .Append(",\"players\":").Append(w.Players >= 0 ? w.Players.ToString(CultureInfo.InvariantCulture) : "null")
          .Append('}');
        return sb.ToString();
    }

    private static void Stat(StringBuilder sb, StepStat s) =>
        sb.Append("{\"avg\":").Append(N(s.Avg, 3)).Append(",\"p95\":").Append(N(s.P95, 3)).Append(",\"max\":").Append(N(s.Max, 3)).Append('}');

    private static string N(double d, int digits) =>
        double.IsFinite(d) ? Math.Round(d, digits).ToString("0.###", CultureInfo.InvariantCulture) : "0";
}
