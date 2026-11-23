// SPDX-License-Identifier: MIT
// KAP/1 additions over a real loopback socket: the "metrics" topic (pushed to its subscribers only), ready.restart,
// the one-time login secret (KAP only), bye{reason: "restart"}.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class KapTests : AdminTest
{
    private readonly Bridge _bridge;
    private readonly TcpAdmin _tcp;
    private readonly string _data;

    public KapTests()
    {
        _data = Path.Combine(Dir, "kiln", "data", "kiln-admin");
        Directory.CreateDirectory(_data);
        Ops.DataDirectory = _data;
        _bridge = new Bridge(Path.Combine(Dir, "bridge"), Log);
        _bridge.EnsureDirectories();
        _tcp = new TcpAdmin(_bridge, Log, _data, () => ("srv-test", "Test"), "0.2.0-test", "0.2.0")
        {
            RestartInfo = new RestartSupport(true, "kilnctl", "run-1").ToJson(),
        };
        Assert.True(_tcp.Start(0, []));
        Ops.Tcp = _tcp;
    }

    public override void Dispose()
    {
        _tcp.Stop("unload");
        Ops.Tcp = null;
        _bridge.Dispose();
        base.Dispose();
    }

    private KapClient Connect(string name = "kiln-admin-tests") => new(_tcp.Port, Path.Combine(_data, AdminToken.FileName), name);

    private static MetricsWindow Window(int players = 3) =>
        new(1_700_000_000_000, 2.0, new StepStat(16.7, 18.2, 20.5, 120), new StepStat(0.7, 2.8, 8.1, 120), players);

    [Fact]
    public void Ready_lists_the_metrics_topic_and_says_whether_a_restart_comes_back()
    {
        using var c = Connect();
        Assert.Contains("metrics", c.Ready["topics"]!.AsArray().Select(n => n!.GetValue<string>()));
        var r = c.Ready["restart"]!;
        Assert.Equal("supported", r["supported"]!.GetValue<string>());
        Assert.Equal("kilnctl", r["via"]!.GetValue<string>());
        Assert.Equal(75, r["code"]!.GetValue<int>());
        Assert.False(c.Hello.ContainsKey("restart"));  // nothing more before the handshake
    }

    [Fact]
    public void Metrics_go_to_subscribers_only_and_only_while_someone_listens()
    {
        Assert.False(_tcp.WantsMetrics);
        using var a = Connect("a");
        using var b = Connect("b");
        Assert.True(a.Sub("metrics")["ok"]!.GetValue<bool>());
        Assert.True(Eventually(() => _tcp.WantsMetrics));
        _tcp.OfferMetrics(Window());
        var push = a.Next(f => f["t"]?.GetValue<string>() == "push" && f["topic"]?.GetValue<string>() == "metrics", 3000);
        Assert.Equal(1, push["seq"]!.GetValue<int>());
        var d = push["data"]!;
        Assert.Equal(1_700_000_000_000, d["at"]!.GetValue<long>());
        Assert.Equal(120, d["steps"]!.GetValue<int>());
        Assert.Equal(60, d["stepsPerS"]!.GetValue<double>());
        Assert.Equal(18.2, d["frameMs"]!["p95"]!.GetValue<double>());
        Assert.Equal(8.1, d["tickMs"]!["max"]!.GetValue<double>());
        Assert.True(d["memory"]!["workingSet"]!.GetValue<long>() > 0);
        Assert.True(d["memory"]!["managedHeap"]!.GetValue<long>() > 0);
        Assert.Equal(Environment.ProcessorCount, d["cpu"]!["cores"]!.GetValue<int>());
        Assert.Equal(3, d["players"]!.GetValue<int>());
        _tcp.OfferMetrics(Window());
        Assert.Equal(2, a.Next(f => f["topic"]?.GetValue<string>() == "metrics", 3000)["seq"]!.GetValue<int>());
        Thread.Sleep(300);
        Assert.Empty(b.Seen(f => f["topic"]?.GetValue<string>() == "metrics"));

        Assert.True(a.Sub("metrics", on: false)["ok"]!.GetValue<bool>());
        Assert.True(Eventually(() => !_tcp.WantsMetrics));
        b.Sub("metrics");
        Assert.True(Eventually(() => _tcp.WantsMetrics));
        b.Dispose();  // a subscriber that goes away
        Assert.True(Eventually(() => !_tcp.WantsMetrics));
    }

    [Fact]
    public void A_login_secret_is_redeemed_once_by_its_consumer_over_KAP_only()
    {
        Ops.RegisterLogin(_bridge, Log, new LoginTokens(() => Environment.TickCount64));
        Assert.Contains("login_mint", _bridge.OpNames);
        using var launcher = Connect("kiln-launcher/0.2.0");
        using var web = Connect("kiln-webadmin/1.0");
        var minted = launcher.Call("login_mint", new JsonObject { ["consumer"] = "kiln-webadmin", ["label"] = "Open web panel" }, () => _tcp.Drain());
        Assert.True(minted["ok"]!.GetValue<bool>(), minted.ToJsonString());
        var secret = minted["data"]!["secret"]!.GetValue<string>();
        Assert.Matches("^[0-9a-f]{64}$", secret);
        Assert.Equal(60, minted["data"]!["ttlS"]!.GetValue<int>());

        var other = web.Call("login_redeem", new JsonObject { ["consumer"] = "someone-else", ["secret"] = secret }, () => _tcp.Drain());
        Assert.False(other["ok"]!.GetValue<bool>());
        var wrong = web.Call("login_redeem", new JsonObject { ["consumer"] = "kiln-webadmin", ["secret"] = new string('0', 64) }, () => _tcp.Drain());
        Assert.False(wrong["ok"]!.GetValue<bool>());
        Assert.Equal("op_failed", wrong["code"]!.GetValue<string>());

        var ok = web.Call("login_redeem", new JsonObject { ["consumer"] = "kiln-webadmin", ["secret"] = secret }, () => _tcp.Drain());
        Assert.True(ok["ok"]!.GetValue<bool>(), ok.ToJsonString());
        Assert.Equal("owner", ok["data"]!["role"]!.GetValue<string>());
        Assert.Equal("kiln-launcher/0.2.0", ok["data"]!["mintedBy"]!.GetValue<string>());
        Assert.Equal("Open web panel", ok["data"]!["label"]!.GetValue<string>());
        var again = web.Call("login_redeem", new JsonObject { ["consumer"] = "kiln-webadmin", ["secret"] = secret }, () => _tcp.Drain());
        Assert.False(again["ok"]!.GetValue<bool>());

        // the file bridge never mints (its result files would keep the secret on disk)
        JsonObject? viaFile = null;
        _bridge.Execute(new JsonObject { ["op"] = "login_mint", ["consumer"] = "kiln-webadmin" }, r => viaFile = r);
        Assert.False(viaFile!["ok"]!.GetValue<bool>());
        Assert.Contains("only served over KAP", viaFile["error"]!.GetValue<string>());
        Assert.DoesNotContain(secret, Backend.All());
        Assert.True(Backend.Logged("login secret minted for kiln-webadmin by kiln-launcher/0.2.0"));
    }

    [Fact]
    public void A_restart_says_so_in_bye()
    {
        using var c = Connect();
        _tcp.Stop("restart");
        Assert.Equal("restart", c.Next(f => f["t"]?.GetValue<string>() == "bye", 3000)["reason"]!.GetValue<string>());
    }
}
