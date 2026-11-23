// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class RestartTests
{
    private sealed class Host : IRestartHost
    {
        public long NowMs { get; set; } = 1_000_000;
        public readonly List<string> Chat = [], Logs = [];
        public readonly List<int> Raised = [];
        public JsonObject? Marker;
        public bool MarkerDeleted;
        public TaskCompletionSource<string?> Save = new();
        public Func<int, string?> Raise = a => a == 1 ? "kernelbase.dll!CtrlRoutine" : "GenerateConsoleCtrlEvent";

        public void Broadcast(string text) => Chat.Add(text);

        public Task<string?> SaveAndMark(JsonObject marker)
        {
            Marker = marker;
            return Save.Task;
        }

        public void DeleteMarker() => MarkerDeleted = true;

        public string? RaiseStop(int attempt)
        {
            Raised.Add(attempt);
            return Raise(attempt);
        }

        public void Log(LogLevel level, string text) => Logs.Add($"[{level}] {text}");
    }

    private static readonly RestartSupport Kilnctl = new(true, "kilnctl", "run-7");

    private static void Run(Host h, RestartController r, int ms, int stepMs = 40)
    {
        for (var t = 0; t < ms; t += stepMs)
        {
            h.NowMs += stepMs;
            r.Tick();
        }
    }

    [Fact]
    public void Counts_down_in_chat_saves_then_raises_the_stop()
    {
        var h = new Host();
        var r = new RestartController(h, Kilnctl);
        var info = r.Schedule(65, "Back in a minute.", "web:alice");
        Assert.Equal("counting", info["state"]!.GetValue<string>());
        Assert.Equal(65, info["secondsLeft"]!.GetValue<int>());
        Assert.Equal("supported", info["restart"]!["supported"]!.GetValue<string>());
        Run(h, r, 64_000);
        Assert.Equal([
            "The server restarts in 65 seconds. Back in a minute.", "The server restarts in 1 minute. Back in a minute.",
            "The server restarts in 30 seconds.", "The server restarts in 10 seconds.", "The server restarts in 5 seconds.",
            "The server restarts in 4 seconds.", "The server restarts in 3 seconds.", "The server restarts in 2 seconds.",
            "The server restarts in 1 second.",
        ], h.Chat);
        Assert.Null(h.Marker);
        Run(h, r, 1000);
        Assert.Equal("The server restarts now: saving the world.", h.Chat[^1]);
        Assert.Equal(RestartState.Saving, r.State);
        Assert.Equal("run-7", h.Marker!["runId"]!.GetValue<string>());
        Assert.Equal("web:alice", h.Marker["by"]!.GetValue<string>());
        Assert.Throws<AdminError>(() => r.Cancel("web:bob"));  // too late
        Assert.Throws<AdminError>(() => r.Schedule(5, null, "web:bob"));  // one at a time
        Run(h, r, 400);
        Assert.Empty(h.Raised);  // still saving
        h.Save.SetResult(null);
        Run(h, r, 40);
        Assert.Equal([1], h.Raised);
        Assert.True(r.Stopping);
        Assert.Contains(h.Logs, l => l.Contains("stopping the server through kernelbase.dll!CtrlRoutine"));
    }

    [Fact]
    public void A_stop_that_does_not_begin_tries_the_console_api_then_gives_up_without_killing()
    {
        var h = new Host();
        var r = new RestartController(h, Kilnctl);
        r.Schedule(0, null, "launcher");
        h.Save.SetResult(null);
        Run(h, r, 200);
        Assert.Equal([1], h.Raised);
        Run(h, r, RestartController.StopWaitMs + 100);
        Assert.Equal([1, 2], h.Raised);
        Run(h, r, RestartController.StopWaitMs + 100);
        Assert.Equal(RestartState.Failed, r.State);
        Assert.True(h.MarkerDeleted);  // so kilnctl never restarts a later, normal stop
        Assert.Equal("The server restart failed; the server keeps running.", h.Chat[^1]);
        Assert.Contains("did not begin to stop", r.Info()!["error"]!.GetValue<string>());
        // and a new one can be asked for
        r.Schedule(10, null, "launcher");
        Assert.Equal(RestartState.Counting, r.State);
    }

    [Fact]
    public void Without_CtrlRoutine_the_console_api_is_next_and_without_both_it_fails()
    {
        var h = new Host { Raise = a => a == 2 ? "GenerateConsoleCtrlEvent" : null };
        var r = new RestartController(h, Kilnctl);
        r.Schedule(0, null, "x");
        h.Save.SetResult(null);
        Run(h, r, 80);
        Assert.Equal([1, 2], h.Raised);
        Assert.True(r.Stopping);

        var none = new Host { Raise = _ => null };
        var r2 = new RestartController(none, Kilnctl);
        r2.Schedule(0, null, "x");
        none.Save.SetResult(null);
        Run(none, r2, 80);
        Assert.Equal(RestartState.Failed, r2.State);
        Assert.True(none.MarkerDeleted);
        Assert.Contains("no way to raise its stop", r2.Info()!["error"]!.GetValue<string>());
    }

    [Fact]
    public void A_failed_save_stops_nothing()
    {
        var h = new Host();
        var r = new RestartController(h, new RestartSupport(false, null, null));
        r.Schedule(3, null, "x");
        Assert.Equal("The server stops in 3 seconds.", h.Chat[0]);  // nothing brings it back: a stop
        Run(h, r, 3100);
        h.Save.SetResult("the disk is full");
        Run(h, r, 80);
        Assert.Equal(RestartState.Failed, r.State);
        Assert.Empty(h.Raised);
        Assert.False(h.MarkerDeleted);  // never written
        Assert.Contains("the disk is full", r.Info()!["error"]!.GetValue<string>());
        Assert.Null(h.Marker!["runId"]);
    }

    [Fact]
    public void Cancel_while_counting_down()
    {
        var h = new Host();
        var r = new RestartController(h, Kilnctl);
        Assert.Throws<AdminError>(() => r.Cancel("x"));  // nothing scheduled
        r.Schedule(600, null, "x");
        Assert.Equal("The server restarts in 10 minutes.", h.Chat[0]);
        Run(h, r, 2000);
        Assert.True(r.Cancel("web:bob")["canceled"]!.GetValue<bool>());
        Assert.Equal("The server restart was canceled.", h.Chat[^1]);
        Run(h, r, 700_000, 1000);
        Assert.Null(h.Marker);
        Assert.Empty(h.Raised);
        Assert.Throws<AdminError>(() => r.Schedule(3601, null, "x"));
    }

    [Theory]
    [InlineData("kilnctl", "r1", true)]
    [InlineData("kilnctl", null, false)]  // kilnctl could not match the marker to its run
    [InlineData(null, "r1", false)]  // kilnctl without systemd: nothing restarts it
    [InlineData("other", "r1", false)]
    public void Restart_is_supported_only_under_kilnctls_unit(string? restart, string? run, bool supported)
    {
        var s = RestartSupport.Detect(k => k switch { "KILN_RESTART" => restart, "KILN_RUN_ID" => run, _ => null });
        Assert.Equal(supported, s.Supported);
        Assert.Equal(supported ? "supported" : "unknown", s.ToJson()["supported"]!.GetValue<string>());
        Assert.Equal(run, s.RunId);
    }

    [Fact]
    public void Spans_read_like_chat()
    {
        Assert.Equal("10 minutes", RestartController.Span(600));
        Assert.Equal("1 minute", RestartController.Span(60));
        Assert.Equal("90 seconds", RestartController.Span(90));
        Assert.Equal("1 second", RestartController.Span(1));
    }
}

public sealed class GameRestartHostTests : AdminTest
{
    [Fact]
    public void An_abandoned_restart_leaves_no_marker()
    {
        var host = new GameRestartHost(Log, Dir);
        var marker = new JsonObject { ["format"] = 1, ["runId"] = "r" };
        // Kiln.Saves is not there in a test: straight to the marker
        Assert.Null(host.SaveAndMark(marker).GetAwaiter().GetResult());
        Assert.True(File.Exists(host.MarkerPath));
        host.Abandon();  // unloaded before the stop was raised
        Assert.False(File.Exists(host.MarkerPath));
        Assert.Equal("Kiln.Admin unloaded", host.SaveAndMark(marker).GetAwaiter().GetResult());
        Assert.False(File.Exists(host.MarkerPath));
        Assert.False(GameRestartHost.DeleteMarker(host.MarkerPath, Log));
    }
}
