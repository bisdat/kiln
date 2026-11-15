// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Network.Tests;

/// <summary>What a refused player reads when the building-material ids differ.</summary>
public class WorldTablesGateTests
{
    private static readonly List<(int Id, string Guid, string Label)> Server =
    [
        (213, "041e7974e9d05097", "brick-first (mod first)"),
        (214, "0bc0dd6153c25d22", "brick-a (mod a)"),
    ];

    [Fact]
    public void NamesAMovedId() =>
        Assert.Contains("brick-a (mod a) is id 214 here and 213 on your game", WorldTablesGate.Describe(Server, "213=0bc0dd6153c25d22"));

    [Fact]
    public void NamesWhatTheClientLacks()
    {
        var text = WorldTablesGate.Describe(Server, "214=0bc0dd6153c25d22");
        Assert.Contains("you don't have brick-first (mod first)", text);
        Assert.DoesNotContain("brick-a", text);
    }

    [Fact]
    public void NamesWhatOnlyTheClientHas() =>
        Assert.Contains("your game has material ffff (id 215), and this server doesn't",
            WorldTablesGate.Describe(Server, "213=041e7974e9d05097,214=0bc0dd6153c25d22,215=ffff"));

    [Fact]
    public void ANoKilnClientLacksAll()
    {
        var text = WorldTablesGate.Describe(Server, "");
        Assert.StartsWith("This server's building and terrain materials don't match your game's:", text);
        Assert.Contains("you don't have brick-first", text);
        Assert.Contains("you don't have brick-a", text);
    }

    // dev.29: the custom material pools by name, when the client sent its own (an older peer sends none).
    private const string SameIds = "213=041e7974e9d05097,214=0bc0dd6153c25d22";
    private static readonly Dictionary<string, string> ServerPools = new() { ["mp-fixture:four"] = "aaaa", ["mp-fixture:roof"] = "bbbb" };

    [Fact]
    public void NamesAPoolTheClientLacks() =>
        Assert.Contains("you don't have the material pool mp-fixture:four (mod mp-fixture is missing, another version, or your Kiln refused the pool",
            WorldTablesGate.Describe(Server, SameIds, ServerPools, "mp-fixture:roof=bbbb"));

    [Fact]
    public void NamesAPoolThatDiffers()
    {
        var text = WorldTablesGate.Describe(Server, SameIds, ServerPools, "mp-fixture:four=cccc,mp-fixture:roof=bbbb");
        Assert.Contains("the material pool mp-fixture:four differs between this server and your game (another version of mod mp-fixture", text);
        Assert.DoesNotContain("mp-fixture:roof", text);
        Assert.DoesNotContain("the lists differ", text);
    }

    [Fact]
    public void NamesAPoolOnlyTheClientHas() =>
        Assert.Contains("your game has the material pool old-mod:walls, and this server doesn't (another version of mod old-mod, or a pool your game kept",
            WorldTablesGate.Describe(Server, SameIds, ServerPools, "mp-fixture:four=aaaa,mp-fixture:roof=bbbb,old-mod:walls=dddd"));

    [Fact]
    public void AnOlderPeerKeepsTodaysText()
    {
        var text = WorldTablesGate.Describe(Server, SameIds, ServerPools, null);
        Assert.Equal(WorldTablesGate.Describe(Server, SameIds), text);
        Assert.Contains("the lists differ", text);
    }

    [Fact]
    public void APoolDigestFollowsOrderRoofAndBinding()
    {
        var g = "d484d837-f2d9-5cef-a617-196245f5db4c";
        var one = WorldTablesGate.PoolDigest(g, false, [1u, 2u], []);
        Assert.Equal(one, WorldTablesGate.PoolDigest(g, false, [1u, 2u], []));
        Assert.NotEqual(one, WorldTablesGate.PoolDigest(g, false, [2u, 1u], []));
        Assert.NotEqual(one, WorldTablesGate.PoolDigest(g, true, [1u, 2u], []));
        Assert.NotEqual(one, WorldTablesGate.PoolDigest(g, false, [1u, 2u], ["41a460a5-d463-5c2d-bf26-1a072d6e84b1"]));
        // the lab runner (mods/tests/material-pools-test/run.py) computes the same digest for an old client's pools
        Assert.Equal("1eb0229c3b458816", WorldTablesGate.PoolDigest(g, false, [1u, 2u], ["41a460a5-d463-5c2d-bf26-1a072d6e84b1"]));
    }
}
