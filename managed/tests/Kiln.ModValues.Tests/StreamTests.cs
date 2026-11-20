// SPDX-License-Identifier: MIT
// The stats stream's reducers (chapter 05b section 4.4) with a fake clock and a fake transport: parts, digests,
// atomic publication, deltas, removals, overflow, nonces, retries, the carry offer, and the client's read policy.
using System.Text;
using System.Text.Json;
using Kiln;
using Xunit;

namespace ModValuesTests;

public sealed class StreamTests
{
    private sealed class Events : ISyncedEvents
    {
        public readonly List<ModValueSyncedEvent> Synced = [];
        public readonly List<ModValuesSnapshotPublishedEvent> Published = [];
        void ISyncedEvents.Synced(ModValueSyncedEvent e) => Synced.Add(e);
        void ISyncedEvents.Published(ModValuesSnapshotPublishedEvent e) => Published.Add(e);
    }

    private sealed class Pair
    {
        public DateTime Now = new(2026, 11, 20, 12, 0, 0, DateTimeKind.Utc);
        public readonly LedgerTests.Rig Rig = new();
        public readonly CharacterKey A;
        public readonly ServerSession Server;
        public readonly ClientStream Client;
        public readonly Events Ev = new();
        public readonly List<object> ToClient = [], ToServer = [];
        public bool AutoDeliver = true;

        public Pair(Action<LedgerTests.Rig> define)
        {
            define(Rig);
            A = Rig.Join(1);
            Rig.Ledger.Dirty += (c, k) => Server?.MarkDirty(k.ToString());
            Server = new ServerSession(A, Rig.Store.World, Now);
            Client = new ClientStream(() => Now, m => ToServer.Add(m), Ev);
            Client.OnHello(Server.Hello());
        }

        public void Pump(int steps = 1)
        {
            for (var i = 0; i < steps; i++)
            {
                Server.Pump(Now, () => Rig.Ledger.Visible(A), m => ToClient.Add(m));
                if (AutoDeliver) Deliver();
                Now += TimeSpan.FromMilliseconds(500);
            }
        }

        public void Deliver()
        {
            foreach (var m in ToClient.ToArray())
            {
                ToClient.Remove(m);
                switch (m)
                {
                    case ModValuesWire.Hello h: Client.OnHello(h); break;
                    case ModValuesWire.StatePart p: Client.OnPart(p); break;
                }
            }
            foreach (var m in ToServer.ToArray())
            {
                ToServer.Remove(m);
                switch (m)
                {
                    case ModValuesWire.Ack a: Server.OnAck(a); break;
                    case ModValuesWire.Resync r: Server.OnResync(r); break;
                }
            }
        }

        public long Raw(string key) => Client.State.Snapshot(key)?.Value ?? -1;
    }

    private static readonly ModValueOptions Coins = new(ModValueKind.Currency, "Coins");
    private static ModValueKey K(string s) => ModValueKey.TryParse(s, out var k) ? k : throw new ArgumentException(s);

    private static void Many(LedgerTests.Rig r)
    {
        for (var i = 0; i < 100; i++)
            r.Define("bulk", $"v{i:d3}", new ModValueOptions(ModValueKind.Counter, UiText.Of(("en", "A rather long label to make the state big " + i), ("de", "Eine ziemlich lange Beschriftung " + i)),
                Initial: 9007199254740993, Sync: ModValueSyncPolicy.Readers, ReadPolicy: ModValueReadPolicy.Public));
    }

    [Fact]
    public void OwnerGetsAFullStreamFirstAndExactValues()
    {
        var p = new Pair(r => r.Define("fish", "coins", Coins));
        p.Pump();
        Assert.Equal(1ul, p.Client.State.Seq);
        Assert.Equal(0, p.Raw("fish:coins"));
        Assert.Single(p.Ev.Published, e => e.Full && e.Sequence == 1);
        p.Rig.Ledger.Change("fish", p.A, K("fish:coins"), ChangeOp.Add, 11, null, 0);
        p.Pump();
        Assert.Equal(11, p.Raw("fish:coins"));
        Assert.Equal(2ul, p.Client.State.Seq);
        Assert.Contains(p.Ev.Synced, e => e.After?.Value == 11 && e.Before?.Value == 0);
    }

    [Fact]
    public void StateOverSixteenKiBTravelsInPartsUnderTheMessageLimit()
    {
        var p = new Pair(Many) { AutoDeliver = false };
        p.Server.Pump(p.Now, () => p.Rig.Ledger.Visible(p.A), m => p.ToClient.Add(m));
        for (var i = 0; i < 40 && p.Server.InFlight && p.ToClient.Count(m => m is ModValuesWire.StatePart) < ((ModValuesWire.StatePart)p.ToClient[0]).Parts; i++)
        {
            p.Now += TimeSpan.FromSeconds(1);
            p.Server.Pump(p.Now, () => p.Rig.Ledger.Visible(p.A), m => p.ToClient.Add(m));
        }
        var parts = p.ToClient.OfType<ModValuesWire.StatePart>().ToList();
        Assert.True(parts.Count > 2, $"{parts.Count} parts");
        Assert.True(parts[0].TotalBytes > 16 * 1024);
        foreach (var part in parts) Assert.True(JsonSerializer.SerializeToUtf8Bytes(part, ModValuesWire.Options).Length <= ModValuesWire.MaxMessageBytes);
        // reversed: nothing is published before the last part, then all at once
        foreach (var part in Enumerable.Reverse(parts).SkipLast(1))
        {
            p.Client.OnPart(part);
            Assert.Empty(p.Client.State.Values);
        }
        p.Client.OnPart(parts[0]);
        Assert.Equal(100, p.Client.State.Values.Count);
        Assert.Equal(9007199254740993, p.Raw("bulk:v042"));
        Assert.Single(p.Ev.Published, e => e.Sequence == 1);
    }

    [Fact]
    public void DuplicatePartsAreIgnoredConflictingOnesResync()
    {
        var p = new Pair(Many) { AutoDeliver = false };
        for (var i = 0; i < 20; i++)
        {
            p.Server.Pump(p.Now, () => p.Rig.Ledger.Visible(p.A), m => p.ToClient.Add(m));
            p.Now += TimeSpan.FromSeconds(1);
        }
        var all = p.ToClient.OfType<ModValuesWire.StatePart>().ToList();
        var parts = all.Take(all[0].Parts).ToList();  // the first round (the server retried the unacknowledged transfer)
        p.Client.OnPart(parts[0]);
        p.Client.OnPart(parts[0]);
        Assert.Empty(p.ToServer);
        p.Client.OnPart(parts[0] with { PayloadB64 = Convert.ToBase64String(Encoding.UTF8.GetBytes("wrong")) });
        Assert.IsType<ModValuesWire.Resync>(Assert.Single(p.ToServer));
        foreach (var part in parts.Skip(1)) p.Client.OnPart(part);
        Assert.Empty(p.Client.State.Values);  // the assembly was dropped: never half a state
    }

    [Fact]
    public void ADeltaOnAnotherBaseIsNeverPublished()
    {
        var p = new Pair(r => r.Define("fish", "coins", Coins));
        p.Pump();
        var s = p.Client.State;
        var doc = JsonSerializer.SerializeToUtf8Bytes(new ModValuesWire.Payload { Values = [new ModValuesWire.Value { Key = "fish:coins", Raw = "999" }] }, ModValuesWire.Options);
        p.Client.OnPart(new ModValuesWire.StatePart(s.Nonce!, "5", "5", "4", false, 0, 1, doc.Length, ModValuesWire.Sha(doc), Convert.ToBase64String(doc)));
        Assert.Equal(0, p.Raw("fish:coins"));
        Assert.IsType<ModValuesWire.Resync>(Assert.Single(p.ToServer));
    }

    [Fact]
    public void AnotherNonceIsDroppedAndANewHelloClearsTheCopy()
    {
        var p = new Pair(r => r.Define("fish", "coins", Coins));
        p.Pump();
        var doc = JsonSerializer.SerializeToUtf8Bytes(new ModValuesWire.Payload(), ModValuesWire.Options);
        p.Client.OnPart(new ModValuesWire.StatePart(new string('b', 32), "1", "1", "0", true, 0, 1, doc.Length, ModValuesWire.Sha(doc), Convert.ToBase64String(doc)));
        Assert.Equal(0, p.Raw("fish:coins"));
        Assert.Empty(p.ToServer);
        p.Client.OnHello(p.Server.Rotate());
        Assert.Empty(p.Client.State.Values);  // cleared before the next frame can show it
        p.Pump();
        Assert.Equal(0, p.Raw("fish:coins"));
    }

    [Fact]
    public void RemovalAndRetiredDefinitions()
    {
        var p = new Pair(r => { r.Define("fish", "coins", Coins); r.Define("fish", "tokens", Coins); });
        p.Pump();
        Assert.Equal(2, p.Client.State.Values.Count);
        p.Rig.Defs.Retire("fish");
        p.Rig.Define("fish", "coins", Coins);
        p.Server.MarkDirty("fish:tokens");
        p.Pump();
        Assert.Equal(["fish:coins"], p.Client.State.Values.Keys);
        Assert.Contains(p.Ev.Synced, e => e.Key.Id == "tokens" && e.After is null);
    }

    [Fact]
    public void DirtyOverflowMakesTheNextTransferFull()
    {
        var p = new Pair(r => r.Define("fish", "coins", Coins));
        p.Pump();
        for (var i = 0; i < ServerSession.DirtyLimit + 5; i++) p.Server.MarkDirty($"x:k{i}");
        p.AutoDeliver = false;
        p.Pump();
        Assert.True(Assert.IsType<ModValuesWire.StatePart>(Assert.Single(p.ToClient)).Full);
    }

    [Fact]
    public void LostAcksRetryTwiceThenAFullAfterTenSeconds()
    {
        var p = new Pair(r => r.Define("fish", "coins", Coins)) { AutoDeliver = false };
        p.Pump();
        Assert.Single(p.ToClient);
        p.ToClient.Clear();
        for (var i = 0; i < 2; i++)
        {
            p.Now += ServerSession.AckTimeout;
            p.Pump();
            var again = Assert.IsType<ModValuesWire.StatePart>(Assert.Single(p.ToClient));
            Assert.Equal("1", again.Seq);  // the same transfer, sequence and digest
            p.ToClient.Clear();
        }
        p.Now += ServerSession.AckTimeout;
        p.Pump();
        Assert.Empty(p.ToClient);  // given up
        Assert.False(p.Server.InFlight);
        p.Now += ServerSession.RetryFullAfter;
        p.Pump();
        var full = Assert.IsType<ModValuesWire.StatePart>(Assert.Single(p.ToClient));
        Assert.True(full.Full);
        Assert.Equal("2", full.Seq);
    }

    [Fact]
    public void ADuplicateTransferIsAcknowledgedAgainWithoutReapplying()
    {
        var p = new Pair(r => r.Define("fish", "coins", Coins)) { AutoDeliver = false };
        p.Pump();
        var part = Assert.IsType<ModValuesWire.StatePart>(Assert.Single(p.ToClient));
        p.Client.OnPart(part);
        p.Client.OnPart(part);
        Assert.Equal(2, p.ToServer.OfType<ModValuesWire.Ack>().Count());
        Assert.Equal(1, p.Client.Commits);
        Assert.Single(p.Ev.Published, e => e.Sequence == 1);
    }

    [Fact]
    public void ClientReadPolicy()
    {
        var p = new Pair(r =>
        {
            r.Define("fish", "own", Coins);
            r.Define("fish", "pub", Coins with { Sync = ModValueSyncPolicy.Readers, ReadPolicy = ModValueReadPolicy.Public });
            r.Define("fish", "ownermod", Coins with { Sync = ModValueSyncPolicy.OwnerMod, ReadPolicy = ModValueReadPolicy.Public });
            r.Define("fish", "list", Coins with { Sync = ModValueSyncPolicy.Readers, ReadPolicy = ModValueReadPolicy.AllowList, ReadMods = ["friend"] });
            r.Define("fish", "secret", Coins with { Sync = ModValueSyncPolicy.None });
        });
        p.Pump();
        var s = p.Client.State;
        Assert.True(ModValuesClient.Read(s, "fish", K("fish:own")).Ok);
        Assert.Equal(ModValueReasons.AccessDenied, ModValuesClient.Read(s, "other", K("fish:own")).Reason);
        Assert.True(ModValuesClient.Read(s, "other", K("fish:pub")).Ok);
        Assert.True(ModValuesClient.Read(s, null, K("fish:pub")).Ok);
        Assert.Equal(ModValueReasons.AccessDenied, ModValuesClient.Read(s, "other", K("fish:ownermod")).Reason);
        Assert.True(ModValuesClient.Read(s, "friend", K("fish:list")).Ok);
        Assert.Equal(ModValueReasons.AccessDenied, ModValuesClient.Read(s, "other", K("fish:list")).Reason);
        Assert.Equal(ModValueReasons.NotSynced, ModValuesClient.Read(s, "fish", K("fish:secret")).Reason);
        p.Client.Clear();
        Assert.Equal(ModValueReasons.NotSynced, ModValuesClient.Read(p.Client.State, "fish", K("fish:own")).Reason);
    }

    [Fact]
    public void ASecondDifferentOfferIsRefusedARepeatGetsTheCachedResult()
    {
        var p = new Pair(r => r.Define("fish", "cx", new ModValueOptions(ModValueKind.Experience, "XP", Thresholds: [0, 10], Persistence: ModValuePersistence.Carried, TrustClientImports: true)));
        p.Pump();
        var proj = "{\"schema\":1,\"partitionRevision\":\"3\",\"entries\":{\"cx\":{\"raw\":\"40\",\"valueRevision\":\"3\",\"definitionVersion\":1,\"kind\":\"experience\",\"scale\":0,\"carryEpoch\":1}}}";
        var parts = p.Client.Offer(3, new Dictionary<string, string> { ["fish"] = proj });
        var part = Assert.Single(parts);
        Assert.Empty(p.Client.Offer(3, new Dictionary<string, string> { ["fish"] = proj }));  // once per nonce
        var sent = new List<object>();
        var doc = p.Server.OnOfferPart(part, p.Now, sent.Add);
        Assert.NotNull(doc);
        var result = p.Server.OfferDone(part.Transfer, part.Sha256, p.Rig.Ledger.Import(p.A, ServerSession.ParseOffer(doc!)!));
        Assert.Equal((true, 1), (result.Ok, result.Applied));
        Assert.Equal(40, p.Rig.Ledger.Read("fish", p.A, K("fish:cx")).Value!.Value);
        Assert.Null(p.Server.OnOfferPart(part, p.Now, sent.Add));
        Assert.Same(result, Assert.Single(sent));  // the cached summary, not a second merge
        Assert.Null(p.Server.OnOfferPart(part with { Transfer = "2", Sha256 = new string('0', 64) }, p.Now, sent.Add));
        var refused = Assert.IsType<ModValuesWire.CarryResult>(sent[^1]);
        Assert.False(refused.Ok);
        Assert.Equal(40, p.Rig.Ledger.Read("fish", p.A, K("fish:cx")).Value!.Value);
        p.Pump(2);
        Assert.Equal(40, p.Raw("fish:cx"));  // the applied offer forced a new full
    }

    [Fact]
    public void WireNumbersAreCanonicalStrings()
    {
        Assert.True(ModValuesWire.TryU64("18446744073709551615", out var max) && max == ulong.MaxValue);
        Assert.False(ModValuesWire.TryU64("01", out _));
        Assert.False(ModValuesWire.TryU64("-1", out _));
        Assert.False(ModValuesWire.TryU64("18446744073709551616", out _));
        Assert.True(ModValuesWire.TryI64("-9223372036854775808", out var min) && min == long.MinValue);
        Assert.False(ModValuesWire.TryI64("-0", out _));
        Assert.False(ModValuesWire.TryI64("+1", out _));
        Assert.False(ModValuesWire.TryI64("1e3", out _));
    }
}
