// SPDX-License-Identifier: MIT
// Step 0 (webadmin/out/step0/REPORT.md section 2.5): Kiln.Admin's Pre handler once took 810 ms while Kiln.Saves waited
// 3.66 s for the disk. What Pre did on the tick thread that can wait for the disk: write state.json (a temp file + a
// rename, every second), scan the file bridge's requests folder (every other step) and write its results, and the ops
// that only read files (log_tail, native_status, crash_reports). None of that may wait for the disk on the tick thread.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class StallTests : AdminTest
{
    [Fact]
    public void Writing_state_json_never_waits_for_the_disk()
    {
        Assert.SkipUnless(OperatingSystem.IsLinux(), "a FIFO blocks open() on Linux");
        var bridge = new Bridge(Dir, Log);
        bridge.EnsureDirectories();
        var tmp = Path.Combine(Dir, ".state.json.tmp");
        Fifo.Make(tmp);
        try
        {
            var first = Task.Run(() => bridge.WriteStateText("{\"n\":1}"));
            Assert.True(first.Wait(2000), "WriteStateText (the tick thread, once a second) waited for a stuck disk");
            var more = Task.Run(() =>
            {
                bridge.WriteStateText("{\"n\":2}");
                bridge.WriteStateText("{\"n\":3}");
            });
            Assert.True(more.Wait(2000), "later state writes waited for a stuck disk");
        }
        finally
        {
            // the I/O thread took the newest text it had when it opened the file: n 1, or 3 if it started late
            Assert.True(Fifo.Release(tmp, read: true).Wait(5000));
        }
        // the disk answers again: the newest state lands (a regular file again; a FIFO has no length, and is never
        // opened here to read: that would wait for a writer)
        bridge.WriteStateText("{\"n\":4}");
        var state = Path.Combine(Dir, "state.json");
        Assert.True(Eventually(() => new FileInfo(state) is { Exists: true, Length: > 0 } && Read(state) == "{\"n\":4}"),
            "state.json did not get the newest state");
        Assert.True(bridge.StatesSkipped >= 1);  // at least one state in between was never written
        ((object)bridge as IDisposable)?.Dispose();
    }

    [Fact]
    public void A_log_tail_waits_for_the_disk_off_the_tick_thread()
    {
        Assert.SkipUnless(OperatingSystem.IsLinux(), "a FIFO blocks open() on Linux");
        var kiln = Path.Combine(Dir, "kiln");
        Directory.CreateDirectory(Path.Combine(kiln, "logs"));
        Ops.DataDirectory = Path.Combine(kiln, "data", "kiln-admin");
        var log = Path.Combine(kiln, "logs", "kiln.log");
        Fifo.Make(log);
        var bridge = new Bridge(Path.Combine(Dir, "bridge"), Log);
        Ops.RegisterAdmin(bridge, Log);
        JsonObject? result = null;
        try
        {
            var call = Task.Run(() => bridge.Execute((JsonObject)JsonNode.Parse("{\"op\":\"log_tail\",\"lines\":5}")!, r => result = r));
            Assert.True(call.Wait(2000), "log_tail read the log on the tick thread");
        }
        finally
        {
            try
            {
                Fifo.Release(log, read: false, "first\nsecond\n").Wait(5000);
            }
            catch (AggregateException)
            {
                // the reader may have gone before the write (EPIPE): the open was all it needed
            }
        }
        Assert.True(Eventually(() =>
        {
            bridge.FinishPending();  // the tick thread delivers what finished
            return result is not null;
        }), "log_tail never answered");
        ((object)bridge as IDisposable)?.Dispose();  // answered (a FIFO cannot be tailed: the outcome is not the point)
    }

    [Fact]
    public void A_log_tail_still_answers_the_last_lines()
    {
        var kiln = Path.Combine(Dir, "kiln");
        Directory.CreateDirectory(Path.Combine(kiln, "logs"));
        Ops.DataDirectory = Path.Combine(kiln, "data", "kiln-admin");
        File.WriteAllText(Path.Combine(kiln, "logs", "kiln.log"), string.Join("\n", Enumerable.Range(1, 50).Select(i => $"line {i}")) + "\n");
        using var bridge = new Bridge(Path.Combine(Dir, "bridge"), Log);
        Ops.RegisterAdmin(bridge, Log);
        JsonObject? result = null;
        bridge.Execute((JsonObject)JsonNode.Parse("{\"op\":\"log_tail\",\"lines\":3}")!, r => result = r);
        Assert.True(Eventually(() =>
        {
            bridge.FinishPending();
            return result is not null;
        }));
        Assert.True(result!["ok"]!.GetValue<bool>(), result.ToJsonString());
        Assert.Equal(["line 48", "line 49", "line 50"], result["data"]!["lines"]!.AsArray().Select(n => n!.GetValue<string>()));
    }

    private static string? Read(string path)
    {
        try
        {
            return File.ReadAllText(path);
        }
        catch (IOException)
        {
            return null;
        }
    }
}
