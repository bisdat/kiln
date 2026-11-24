// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Rules.Tests;

public sealed class CreativeEffectsNetTests
{
    private static readonly EntityId P = new(77);
    private static readonly CreativeStateMsg Legacy = new(CreativeAccess.On, TimeCycle.Normal, 12, "rules", "hooks");
    private static byte[] State(params byte[] suffix)
    {
        var w = new NetWriter();
        CreativeNet.EncodeState(Legacy, w);
        foreach (var b in suffix) w.Byte(b);
        return w.ToArray();
    }

    [Theory]
    [InlineData(0)] [InlineData(1)] [InlineData(2)] [InlineData(3)]
    public void OldServerStateWithTransportPaddingDoesNotAdvertiseEffects(int padding)
    {
        var reader = new NetReader(State(new byte[padding]));
        var decoded = CreativeNet.DecodeState(ref reader);
        Assert.Null(decoded.Effects);
        CreativeMenu.OnState(decoded);
        try
        {
            Assert.False(CreativeMenu.SupportsEffects);
            Assert.False(CreativeNet.Ask(new(CreativeAsk.Effect, Item: 13, Count: 0)));
        }
        finally { CreativeMenu.OnDisconnected(); }
    }

    [Fact]
    public void NewBothRoundTripsIdsFlagsAndNullableLocksWhileOldClientReadsUnchangedPrefix()
    {
        var effects = CreativeEffects.All.Select(e => new CreativeEffectState(e.Name, e.Label, e.Id % 2 == 0,
            "effective", "restore", e.Id % 3 == 0 ? "server denied" : null, e.Id % 4 == 0)).ToArray();
        var w = new NetWriter();
        CreativeNet.EncodeState(Legacy with { Effects = effects }, w);
        w.Byte(0).Byte(0).Byte(0);
        var reader = new NetReader(w.ToArray());
        Assert.Equal(effects, CreativeNet.DecodeState(ref reader).Effects);
        var old = new NetReader(w.ToArray());
        Assert.Equal((uint)Legacy.Access, old.UInt32());
        Assert.Equal((byte)Legacy.Cycle, old.Byte());
        Assert.Equal(Legacy.Hours, old.Double());
        Assert.Equal(Legacy.Rules, old.String());
        Assert.Equal(Legacy.Hooks, old.String());
        Assert.True(old.Remaining > 0); // historical decoder deliberately does not consume the optional suffix
    }

    [Theory]
    [InlineData(new byte[] { 0xce })]
    [InlineData(new byte[] { 0xce, 1, 1 })]
    [InlineData(new byte[] { 0, 0, 0, 0 })]
    [InlineData(new byte[] { 0xce, 2, 1, 1 })]
    [InlineData(new byte[] { 0xce, 1, 23, 1 })]
    [InlineData(new byte[] { 0xce, 1, 0, 0 })]
    [InlineData(new byte[] { 0xce, 1, 1, 255 })]
    [InlineData(new byte[] { 0xce, 1, 1, 13, 2, 0 })]
    [InlineData(new byte[] { 0xce, 1, 1, 13, 0, 2 })]
    public void MalformedSuffixFailsClosed(byte[] suffix) =>
        Assert.Throws<NetFormatException>(() => { var r = new NetReader(State(suffix)); CreativeNet.DecodeState(ref r); });

    [Fact]
    public void DuplicateIdsOversizedDisplaysAndNonPaddingTailAreRejected()
    {
        var duplicate = new NetWriter();
        CreativeNet.EncodeState(Legacy, duplicate);
        duplicate.Byte(0xce).Byte(1).Byte(2);
        for (var i = 0; i < 2; i++) duplicate.Byte(13).Byte(0).Byte(0).String("effective").String("restore").String(null);
        Assert.Throws<NetFormatException>(() => { var r = new NetReader(duplicate.ToArray()); CreativeNet.DecodeState(ref r); });
        var big = new NetWriter();
        CreativeNet.EncodeState(Legacy, big);
        big.Byte(0xce).Byte(1).Byte(1).Byte(13).Byte(0).Byte(0).String(new string('x', 513)).String("").String("");
        Assert.Throws<NetFormatException>(() => { var r = new NetReader(big.ToArray()); CreativeNet.DecodeState(ref r); });
        var trailing = new NetWriter();
        CreativeNet.EncodeState(Legacy with { Effects = [new("stamina", "Stamina", false, "off", "1", null, false)] }, trailing);
        trailing.Byte(7);
        Assert.Throws<NetFormatException>(() => { var r = new NetReader(trailing.ToArray()); CreativeNet.DecodeState(ref r); });
    }

    [Fact]
    public void ValidRequestsUseExistingLayoutAndTheSameAuthenticatedBoundedInbox()
    {
        var w = new NetWriter().Byte(9).UInt32(13).Int32(1).Int32(-1).Int32(-1).Double(0).Int32(0).UInt64(0);
        var reader = new NetReader(w.ToArray());
        var req = CreativeNet.DecodeRequest(ref reader);
        Assert.True(CreativeNet.ValidEffectRequest(req));
        var inbox = new CreativeInbox();
        for (var i = 0; i < 4; i++) Assert.True(inbox.Enqueue(P, req, false, at: 0));
        Assert.False(inbox.Enqueue(P, req, false, at: 0));
        Assert.True(inbox.TryDequeue(out var entry));
        Assert.Equal(P, entry.Player);
        Assert.Equal(req, entry.Req);
        foreach (var bad in new[] { req with { Item = 255 }, req with { Count = 2 }, req with { Count = -1 },
            req with { Value = double.NaN }, req with { Level = 0 }, req with { Rarity = 0 }, req with { Options = 1 }, req with { Slot = 1 } })
        {
            Assert.False(CreativeNet.ValidEffectRequest(bad));
            Assert.False(inbox.Enqueue(P, bad, false, at: 1000));
        }
    }

    [Fact]
    public void MenuRowsShowUnlockedLockedAndOtherOwnerStatesAndDisconnectForgetsSupport()
    {
        var state = new CreativeEffectState("stamina", "Stamina", false, "minStamina=1", "minStamina=0.5", null, true);
        var row = CreativeMenu.EffectRow(state, true);
        Assert.True(row.Enabled);
        Assert.Equal("Creative: off", row.Button);
        Assert.Contains("Effective: minStamina=1", row.Detail);
        Assert.Contains("Other owners", row.Detail);
        var locked = CreativeMenu.EffectRow(state with { Locked = "excluded by preset" }, true);
        Assert.False(locked.Enabled);
        Assert.Contains("Locked for on: excluded by preset", locked.Detail);
        Assert.True(CreativeMenu.EffectRow(state with { Requested = true, Locked = "policy denied" }, true).Enabled); // may withdraw
        Assert.False(CreativeMenu.EffectRow(state, false).Enabled);
        CreativeMenu.OnState(Legacy with { Effects = [state] });
        Assert.True(CreativeMenu.SupportsEffects);
        CreativeMenu.OnDisconnected();
        Assert.False(CreativeMenu.SupportsEffects);
    }
}
