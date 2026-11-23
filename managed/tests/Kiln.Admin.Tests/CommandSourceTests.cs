// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class CommandSourceTests
{
    private static JsonObject Req(string json) => (JsonObject)JsonNode.Parse(json)!;

    [Fact]
    public void Without_source_and_level_a_line_runs_as_today()
    {
        var src = Ops.RemoteSource(Req("{}"));
        Assert.Equal("dashboard", src.Name);
        Assert.Equal(CommandSourceKind.Remote, src.Kind);
        Assert.Equal("remote dashboard", src.ToString());  // no cap
        Assert.True(src.IsAdmin);
    }

    [Fact]
    public void A_named_web_user_runs_capped_at_its_level()
    {
        var mod = Ops.RemoteSource(Req("{\"source\":\"web:alice\",\"level\":\"everyone\"}"));
        Assert.Equal("web:alice", mod.Name);
        Assert.Equal("remote web:alice (up to everyone)", mod.ToString());
        Assert.False(mod.IsAdmin);
        var admin = Ops.RemoteSource(Req("{\"source\":\"web:bob\",\"level\":\"Admin\"}"));
        Assert.Equal("remote web:bob (up to admin)", admin.ToString());
        Assert.True(admin.IsAdmin);
        Assert.Equal("dashboard", Ops.RemoteSource(Req("{\"level\":\"admin\"}")).Name);
        var asPlayer = Ops.LineSource(Req("{\"steamId\":\"76561190000000001\",\"source\":\"web:alice\",\"level\":\"everyone\"}"));
        Assert.Equal(CommandSourceKind.Player, asPlayer.Kind);
        Assert.Equal(76561190000000001UL, asPlayer.SteamId);
        Assert.EndsWith("(up to everyone)", asPlayer.ToString());
    }

    [Theory]
    [InlineData("{\"level\":\"owner\"}", "level: everyone, admin or console")]
    [InlineData("{\"source\":\"web:alice\"}", "source needs a level (everyone, admin or console)")]
    [InlineData("{\"source\":\"web:\\nx\",\"level\":\"admin\"}", "source must be one line")]
    [InlineData("{\"source\":7,\"level\":\"admin\"}", "source must be text")]
    public void Bad_sources_and_levels_are_refused(string json, string error) =>
        Assert.Equal(error, Assert.Throws<AdminError>(() => Ops.RemoteSource(Req(json))).Message);
}
