// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class MetricsTests
{
    private static long At(double ms) => (long)(ms * Stopwatch.Frequency / 1000.0);

    [Fact]
    public void Nobody_listening_costs_nothing()
    {
        var s = new MetricsSampler(100);
        var asked = 0;
        for (var i = 0; i < 50; i++) Assert.Null(s.Pre(false, 16, At(i * 16), () => ++asked));
        s.Post(At(1000));
        Assert.False(s.Running);
        Assert.Equal(0, asked);
    }

    [Fact]
    public void A_window_has_avg_p95_and_max_of_frame_and_tick_time()
    {
        var s = new MetricsSampler(1000);
        var t = 0.0;
        Assert.Null(s.Pre(true, 99, At(t), () => 7));  // the first step only starts the window
        MetricsWindow? w = null;
        for (var i = 1; i <= 40 && w is null; i++)
        {
            s.Post(At(t + (i == 20 ? 30 : 2)));  // the step itself: 2 ms, one of 30 ms
            t += 25;
            w = s.Pre(true, 25, At(t), () => 7);
        }
        Assert.NotNull(w);
        Assert.Equal(40, w!.Frame.Count);
        Assert.Equal(25, w.Frame.Avg, 3);
        Assert.Equal(25, w.Frame.Max, 3);
        Assert.Equal(40, w.Tick.Count);
        Assert.Equal(30, w.Tick.Max, 3);
        Assert.Equal(2, w.Tick.P95, 3);
        Assert.Equal((39 * 2 + 30) / 40.0, w.Tick.Avg, 3);
        Assert.Equal(7, w.Players);
        Assert.Equal(1.0, w.Seconds, 3);
        // the next window starts empty; stopping drops it
        Assert.Null(s.Pre(true, 25, At(t + 25), () => 7));
        Assert.Null(s.Pre(false, 25, At(t + 50), () => 7));
        Assert.False(s.Running);
    }

    [Fact]
    public void The_push_data_is_one_json_object()
    {
        var w = new MetricsWindow(123, 2.004, new StepStat(16.66666, 18, 20, 120), StepStat.None, -1);
        var probe = new ProcessProbe();
        probe.Read();
        Thread.Sleep(50);
        var d = JsonNode.Parse(MetricsJson.Data(w, probe.Read()))!.AsObject();
        Assert.Equal(16.667, d["frameMs"]!["avg"]!.GetValue<double>());
        Assert.Null(d["tickMs"]);  // no Post samples
        Assert.Null(d["players"]);  // unknown
        Assert.NotNull(d["cpu"]!["percent"]);
        Assert.True(d["gcPauseMs"]!.GetValue<double>() >= 0);
    }
}
