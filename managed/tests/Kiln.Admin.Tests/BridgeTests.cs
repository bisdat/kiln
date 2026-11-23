// SPDX-License-Identifier: MIT
// The file bridge with its disk work on its own thread: requests still run once each on the caller's (tick) thread,
// results and state.json are written by the I/O thread, a slow disk never holds up Poll or WriteStateText.
using System.Diagnostics;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class BridgeTests : AdminTest
{
    private Bridge NewBridge()
    {
        var b = new Bridge(Dir, Log);
        b.EnsureDirectories();
        b.Register("echo", req => new JsonObject { ["x"] = req["x"]?.DeepClone(), ["thread"] = Environment.CurrentManagedThreadId });
        return b;
    }

    private string Requests => Path.Combine(Dir, "requests");
    private string Results => Path.Combine(Dir, "results");

    private static JsonObject? ReadJson(string path)
    {
        try
        {
            return File.Exists(path) ? JsonNode.Parse(File.ReadAllText(path)) as JsonObject : null;
        }
        catch (Exception e) when (e is IOException or System.Text.Json.JsonException)
        {
            return null;
        }
    }

    [Fact]
    public void A_request_file_runs_once_on_the_polling_thread_and_gets_its_result_file()
    {
        using var b = NewBridge();
        File.WriteAllText(Path.Combine(Requests, "a1.json"), "{\"op\":\"echo\",\"x\":7}");
        File.WriteAllText(Path.Combine(Requests, "b2.json"), "{not json");
        File.WriteAllText(Path.Combine(Requests, "c3.json"), "{\"op\":\"nope\"}");
        var ran = 0;
        Assert.True(Eventually(() => (ran += b.Poll()) >= 3), $"polled {ran}");
        var a = Path.Combine(Results, "a1.json");
        Assert.True(Eventually(() => ReadJson(a) is not null));
        var r = ReadJson(a)!;
        Assert.True(r["ok"]!.GetValue<bool>());
        Assert.Equal(7, r["data"]!["x"]!.GetValue<int>());
        Assert.Equal(Environment.CurrentManagedThreadId, r["data"]!["thread"]!.GetValue<int>());
        Assert.True(Eventually(() => ReadJson(Path.Combine(Results, "b2.json")) is not null));
        Assert.StartsWith("bad request", ReadJson(Path.Combine(Results, "b2.json"))!["error"]!.GetValue<string>());
        Assert.True(Eventually(() => ReadJson(Path.Combine(Results, "c3.json")) is not null));
        Assert.Equal("unknown op nope", ReadJson(Path.Combine(Results, "c3.json"))!["error"]!.GetValue<string>());
        Assert.Empty(Directory.GetFiles(Requests));
        Assert.Equal(0, b.Poll());  // each ran once
    }

    [Fact]
    public void Poll_and_state_never_wait_for_a_slow_disk()
    {
        using var gate = new ManualResetEventSlim(false);
        using var b = NewBridge();
        var written = new List<string>();
        b.WriteFile = (path, text) =>
        {
            gate.Wait(10_000);
            lock (written) written.Add(Path.GetFileName(path) + " " + text);
            Bridge.WriteAtomic(path, text);
        };
        b.WriteStateText("{\"n\":0}");
        Thread.Sleep(200);  // the I/O thread takes it and is stuck in that write
        var sw = Stopwatch.StartNew();
        var worst = 0L;
        for (var i = 1; i < 100; i++)
        {
            var t0 = sw.ElapsedMilliseconds;
            Assert.Equal(0, b.Poll());
            b.WriteStateText($"{{\"n\":{i}}}");
            worst = Math.Max(worst, sw.ElapsedMilliseconds - t0);
            if (i == 50) File.WriteAllText(Path.Combine(Requests, "r1.json"), "{\"op\":\"echo\",\"x\":1}");
            Thread.Sleep(5);
        }
        Assert.True(worst < 100, $"a Poll or WriteStateText took {worst} ms while the disk was stuck");
        lock (written) Assert.Empty(written);
        gate.Set();
        // the disk answers: the request runs, its result and the newest state are written; most states never were
        var ran = 0;
        Assert.True(Eventually(() => (ran += b.Poll()) == 1));
        Assert.True(Eventually(() => ReadJson(Path.Combine(Dir, "state.json"))?["n"]?.GetValue<int>() == 99));
        Assert.True(Eventually(() => ReadJson(Path.Combine(Results, "r1.json")) is not null));
        lock (written) Assert.True(written.Count(w => w.StartsWith("state.json", StringComparison.Ordinal)) <= 3, string.Join("\n", written));
        Assert.True(b.StatesSkipped > 90);
    }

    [Fact]
    public void Dispose_writes_the_last_state_and_waits_for_a_stuck_disk_only_briefly()
    {
        var b = NewBridge();
        b.WriteStateText("{\"n\":1}");
        b.Dispose();
        Assert.Equal(1, ReadJson(Path.Combine(Dir, "state.json"))!["n"]!.GetValue<int>());

        using var gate = new ManualResetEventSlim(false);
        var stuck = new Bridge(Path.Combine(Dir, "other"), Log);
        stuck.EnsureDirectories();
        stuck.WriteFile = (path, text) => gate.Wait(10_000);
        stuck.WriteStateText("{\"n\":2}");
        Thread.Sleep(200);
        var sw = Stopwatch.StartNew();
        Assert.False(stuck.StopIo(300));
        Assert.True(sw.ElapsedMilliseconds < 2000);
        Assert.True(Backend.Logged("still waiting for the disk"));
        gate.Set();
    }

    [Fact]
    public void A_request_read_but_never_run_gets_a_failed_result_at_the_stop()
    {
        var b = NewBridge();
        var request = Path.Combine(Requests, "u1.json");
        File.WriteAllText(request, "{\"op\":\"echo\",\"x\":1}");
        b.WriteStateText("{\"n\":0}");  // starts the I/O thread, which reads the request; the tick thread never polls again
        Assert.True(Eventually(() => !File.Exists(request)), "the I/O thread did not take the request");
        b.Dispose();
        var r = ReadJson(Path.Combine(Results, "u1.json"));
        Assert.NotNull(r);
        Assert.False(r!["ok"]!.GetValue<bool>());
        Assert.Equal("echo", r["op"]!.GetValue<string>());
        Assert.Contains("unloaded before it ran", r["error"]!.GetValue<string>(), StringComparison.Ordinal);
        // after the stop nothing reads the folder: a request for the next Kiln.Admin stays where it is
        var next = Path.Combine(Requests, "n2.json");
        File.WriteAllText(next, "{\"op\":\"echo\",\"x\":2}");
        Thread.Sleep(300);
        Assert.True(File.Exists(next));
    }

    [Fact]
    public void A_stop_during_a_stuck_write_takes_no_request_after_it()
    {
        using var gate = new ManualResetEventSlim(false);
        var b = NewBridge();
        b.WriteFile = (path, text) =>
        {
            gate.Wait(10_000);
            Bridge.WriteAtomic(path, text);
        };
        b.WriteStateText("{\"n\":1}");
        Thread.Sleep(200);  // the I/O thread is stuck in that write
        var late = Path.Combine(Requests, "late.json");
        File.WriteAllText(late, "{\"op\":\"echo\",\"x\":3}");
        Assert.False(b.StopIo(200));  // the unload goes on without it
        gate.Set();
        Assert.True(b.StopIo(5000), "the I/O thread did not end after its write");
        Assert.True(File.Exists(late), "a request meant for the next Kiln.Admin was taken by the stopped one");
        Assert.False(File.Exists(Path.Combine(Results, "late.json")));
    }

    [Fact]
    public void A_slow_write_is_logged_once()
    {
        using var b = NewBridge();
        b.WriteFile = (path, text) => Thread.Sleep(1100);
        b.WriteStateText("{\"n\":1}");
        Assert.True(Eventually(() => Backend.Logged("writing state.json took"), 5000));
        b.WriteStateText("{\"n\":2}");
        Thread.Sleep(1500);
        lock (Backend.Lines) Assert.Single(Backend.Lines, l => l.Contains("took", StringComparison.Ordinal));
    }
}
