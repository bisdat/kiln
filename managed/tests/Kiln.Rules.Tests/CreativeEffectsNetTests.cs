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

    private static CreativeStateMsg Decode(byte[] bytes)
    {
        var r = new NetReader(bytes);
        return CreativeNet.DecodeState(ref r);
    }

    // The v1 suffix written by hand: marker, version, count, then rows (id, requested, others, three strings).
    private static byte[] Suffix(byte version, int count, params (byte Id, byte Requested, byte Others, string Locked)[] rows)
    {
        var w = new NetWriter();
        CreativeNet.EncodeState(Legacy, w);
        w.Byte(0xce).Byte(version).Byte((byte)count);
        foreach (var (id, requested, others, locked) in rows)
            w.Byte(id).Byte(requested).Byte(others).String("effective " + id).String("restore " + id).String(locked);
        return w.ToArray();
    }

    private static void StateWithoutEffects(CreativeStateMsg decoded)
    {
        Assert.Equal(Legacy, decoded);
        CreativeMenu.OnState(decoded);
        try
        {
            Assert.False(CreativeMenu.SupportsEffects);
            Assert.False(CreativeNet.Ask(new(CreativeAsk.Effect, Item: 13, Count: 1)));
        }
        finally { CreativeMenu.OnDisconnected(); }
    }

    // A suffix this client cannot use never costs the state message: the menu shows, without effect rows.
    [Theory]
    [InlineData(new byte[] { 0xce })]                      // truncated marker
    [InlineData(new byte[] { 0xce, 1, 1 })]                // truncated header
    [InlineData(new byte[] { 0, 0, 0, 0 })]                // no marker
    [InlineData(new byte[] { 0xab, 1, 1, 13 })]            // another marker
    [InlineData(new byte[] { 0xce, 2, 1, 1 })]             // a newer suffix version
    [InlineData(new byte[] { 0xce, 9, 0xff, 0xff, 0xff })] // a newer version with a layout this client cannot read
    [InlineData(new byte[] { 0xce, 1, 23, 1 })]            // more rows than sent
    [InlineData(new byte[] { 0xce, 1, 0, 0 })]             // empty list
    [InlineData(new byte[] { 0xce, 1, 1, 255 })]           // a truncated row
    [InlineData(new byte[] { 0xce, 1, 1, 13, 2, 0 })]      // bad flags, truncated
    [InlineData(new byte[] { 0xce, 1, 1, 13, 0, 2 })]
    public void UnusableSuffixKeepsTheStateWithoutEffectRows(byte[] suffix) => StateWithoutEffects(Decode(State(suffix)));

    [Fact]
    public void NewerServerEffectsAreSkippedAndKnownRowsStillShow()
    {
        // More than 22 effects: every known one plus three this Kiln does not know, interleaved.
        var rows = CreativeEffects.All.Select(e => (e.Id, (byte)(e.Id % 2), (byte)0, e.Id % 3 == 0 ? "locked " + e.Id : ""))
            .Concat([((byte)23, (byte)1, (byte)1, ""), ((byte)24, (byte)0, (byte)0, "future lock"), ((byte)200, (byte)1, (byte)0, "")])
            .OrderBy(r => r.Item1 % 5).ToArray();
        var decoded = Decode(Suffix(1, rows.Length, rows));
        Assert.Equal(Legacy, decoded with { Effects = null });
        Assert.NotNull(decoded.Effects);
        Assert.Equal(CreativeEffects.All.Length, decoded.Effects!.Count);
        foreach (var e in decoded.Effects)
        {
            var id = CreativeEffects.Find(e.Name)!.Id;
            Assert.Equal(id % 2 == 1, e.Requested);
            Assert.Equal("effective " + id, e.Effective);
            Assert.Equal(id % 3 == 0 ? "locked " + id : null, e.Locked);
        }
        var one = Decode(Suffix(1, 2, ((byte)23, (byte)1, (byte)1, ""), ((byte)13, (byte)0, (byte)1, "")));
        Assert.Equal(new[] { "stamina" }, one.Effects!.Select(e => e.Name));
        CreativeMenu.OnState(one);
        try { Assert.True(CreativeMenu.SupportsEffects); }
        finally { CreativeMenu.OnDisconnected(); }
        // Only effects this Kiln does not know: the menu shows without rows.
        StateWithoutEffects(Decode(Suffix(1, 1, ((byte)23, (byte)1, (byte)0, ""))));
    }

    // Malformed or hostile rows keep their limits: nothing from them is shown, but the state still arrives.
    [Fact]
    public void MalformedOrOversizedRowsDropOnlyTheEffectRows()
    {
        StateWithoutEffects(Decode(Suffix(1, 2, ((byte)13, (byte)0, (byte)0, ""), ((byte)13, (byte)0, (byte)0, ""))));   // duplicate
        StateWithoutEffects(Decode(Suffix(1, 2, ((byte)23, (byte)0, (byte)0, ""), ((byte)23, (byte)0, (byte)0, ""))));   // duplicate unknown
        StateWithoutEffects(Decode(Suffix(1, 1, ((byte)23, (byte)2, (byte)0, ""))));                                  // bad flag, unknown row
        StateWithoutEffects(Decode(Suffix(1, 1, ((byte)13, (byte)0, (byte)0, new string('x', 513)))));               // display over 512
        var big = new NetWriter();
        CreativeNet.EncodeState(Legacy, big);
        big.Byte(0xce).Byte(1).Byte(1).Byte(13).Byte(0).Byte(0).String(new string('x', 2049)).String("").String("");
        StateWithoutEffects(Decode(big.ToArray()));                                                                  // over 2048 bytes
        var huge = new NetWriter();
        CreativeNet.EncodeState(Legacy, huge);
        huge.Byte(0xce).Byte(1).Byte(1).Byte(13).Byte(0).Byte(0).VarUInt(1u << 30);                                  // a 1 GiB length claim
        StateWithoutEffects(Decode(huge.ToArray()));
        var trailing = new NetWriter();
        CreativeNet.EncodeState(Legacy with { Effects = [new("stamina", "Stamina", false, "off", "1", null, false)] }, trailing);
        trailing.Byte(7);
        StateWithoutEffects(Decode(trailing.ToArray()));
    }

    [Fact]
    public void ABrokenStatePrefixIsStillABrokenMessage()
    {
        var bytes = State();
        Assert.Throws<NetFormatException>(() => Decode(bytes[..^2]));
    }

    [Fact]
    public void UnusableSuffixesAreLoggedAtMostOnceAMinute()
    {
        var lines = new List<string>();
        var now = 1_000L;
        var (clock, log) = (CreativeNet.SuffixClock, CreativeNet.SuffixLog);
        CreativeNet.SuffixClock = () => now;
        CreativeNet.SuffixLog = lines.Add;
        CreativeNet.ResetSuffixNotes();
        try
        {
            var newer = State(0xce, 2, 1, 1);
            for (var i = 0; i < 3; i++) Assert.Equal(Legacy, Decode(newer));
            now += 59_999;
            Decode(newer);
            Assert.Single(lines);
            Assert.Contains("version 2", lines[0]);
            now += 1;
            Decode(Suffix(1, 2, ((byte)23, (byte)1, (byte)1, ""), ((byte)13, (byte)0, (byte)1, "")));
            Assert.Equal(2, lines.Count);
            Assert.Contains("does not know", lines[1]);
            Assert.Contains("3 more", lines[1]);
            // A well-formed list of known effects and an older server's padding are not notes.
            Decode(State(0, 0, 0));
            now += 60_000;
            var w = new NetWriter();
            CreativeNet.EncodeState(Legacy with { Effects = [new("stamina", "Stamina", false, "off", "1", null, false)] }, w);
            Decode(w.ToArray());
            Assert.Equal(2, lines.Count);
        }
        finally
        {
            (CreativeNet.SuffixClock, CreativeNet.SuffixLog) = (clock, log);
            CreativeNet.ResetSuffixNotes();
        }
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
