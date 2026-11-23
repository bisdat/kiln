// SPDX-License-Identifier: MIT
// Player storage's client copy (0.2.0-dev.30.7, GitHub issues 127 and 128): the kiln:storage wire and its limits, the
// game's copy, the server's passes (owner only, on change, again when not acknowledged), a hosting game's own copy,
// PlayerStorage.Client.ProvideAmmo's two halves (the HUD rows on the kit's fake kiln.ui.ammo, the server's pick check)
// and PlayerStorage.Activate's refusals. A loopback stands in for kiln.net: every update is written by the real encoder
// and read by the real decoder into a real copy per player. The game's firing from a storage cell, the native equip
// from a storage slot and the network itself are the owner's in-game check.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed class PlayerStorageMirrorTests
{
    private const ulong AdaSteam = 76561198000000128, BoSteam = 76561198000000256;

    private static ModContext Mod(string id) =>
        HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", Path.GetTempPath()), Backend.Current.OwnerCreate("mod:" + id), Path.GetTempPath());

    private static T As<T>(ModContext? mod, Func<T> f)
    {
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try { return f(); }
        finally { HostAccess.Current = previous; }
    }

    /// <summary>Stands in for kiln.net, kiln.players and kiln.client.</summary>
    private sealed class Loop : IStorageMirrorLink
    {
        public readonly List<StorageMirrorViewer> Now = [];
        public readonly Dictionary<uint, StorageMirrorStore> Games = new();
        public readonly Dictionary<uint, List<StorageMirrorUpdate>> Sent = new();
        public readonly HashSet<uint> NoChannel = [];
        public readonly List<StorageAmmoPick> Picks = [];
        public bool Hosting { get; set; }
        public EntityId LocalPlayer { get; set; }

        public void See(FakePlayer p, bool local = false)
        {
            Now.Add(new StorageMirrorViewer(p.Entity, p.SteamId, local));
            Games[p.Entity.Value] = new StorageMirrorStore();
            Sent[p.Entity.Value] = [];
        }

        public IReadOnlyList<StorageMirrorViewer> Viewers() => [.. Now];

        public bool CanSend(EntityId player) => !NoChannel.Contains(player.Value);

        public bool Send(EntityId player, StorageMirrorUpdate update)
        {
            if (NoChannel.Contains(player.Value)) return false;
            var w = new NetWriter();
            StorageMirrorUpdate.Encode(update, w);
            Assert.True(w.Length <= StorageMirrorUpdate.MaxBytes);
            var r = new NetReader(w.WrittenSpan);
            var read = StorageMirrorUpdate.Decode(ref r);
            Sent[player.Value].Add(read);
            Games[player.Value].Apply(read);
            return true;
        }

        public bool SendPick(StorageAmmoPick pick)
        {
            Picks.Add(pick);
            return true;
        }
    }

    private sealed class Run : IDisposable
    {
        public readonly KilnHarness Kiln = new();
        public readonly FakePlayer Ada, Bo;
        public readonly ModContext Quivers;
        public readonly ItemId Arrow, FireArrow, Spell, Logs;
        public readonly Loop Link = new();
        public long Now = 1_000_000;
        public readonly List<PlayerStorageClientChangedEvent> Changes = [];
        private IDisposable? _sub;

        public Run()
        {
            Arrow = Kiln.Items.AddItem("Ammo_Arrow_T1", 99, ItemCategory.Ammunition);
            Kiln.Items.SetEquipmentSlotType(Arrow, EquipmentSlot.Arrow);
            FireArrow = Kiln.Items.AddItem("Ammo_Arrow_Fire", 99, ItemCategory.Ammunition);
            Kiln.Items.SetEquipmentSlotType(FireArrow, EquipmentSlot.Arrow);
            Spell = Kiln.Items.AddItem("Ammo_Spell_Fire", 99, ItemCategory.Ammunition);
            Kiln.Items.SetEquipmentSlotType(Spell, EquipmentSlot.Spell);
            Logs = Kiln.Items.AddItem("Material_T1_Wood_Logs", 50, ItemCategory.Materials);
            Ada = Kiln.Players.Join("Ada", AdaSteam, position: new WorldPosition(0, 0, 0));
            Bo = Kiln.Players.Join("Bo", BoSteam, position: new WorldPosition(5, 0, 0));
            Kiln.Step(2);
            Quivers = Mod("quivers");
            PlayerStorageMirror.UseLinkForTests(Link);
            PlayerStorageMirror.Clock = () => Now;
            Kiln.Run(() => _sub = Events.On<PlayerStorageClientChangedEvent>(Changes.Add));
        }

        public PlayerStorage Register(string key, int slots, StorageFilter filter = default) =>
            Kiln.Run(() => As(Quivers, () => Items.RegisterPlayerStorage(key, slots, filter)));

        public EntityId Holder(PlayerStorage s, FakePlayer p) => Kiln.Run(() => s.EntityOf(p.Entity));

        public void Give(PlayerStorage s, FakePlayer p, ItemId item, int count)
        {
            var holder = Holder(s, p);
            var give = Kiln.Run(() => Items.Give(holder, item, count, null, GiveOptions.NoMerge));
            Kiln.Step();
            Assert.Equal(ItemsStatus.Done, give.Result.Status);
        }

        /// <summary>One pass of the server, <paramref name="ms"/> after the last.</summary>
        public void Pass(long ms = 300)
        {
            Now += ms;
            Kiln.Run(PlayerStorageMirror.Pass);
        }

        public void Dispose()
        {
            Kiln.Run(() => _sub?.Dispose());
            Kiln.Dispose();
        }
    }

    private static StorageMirrorUpdate Decode(byte[] bytes)
    {
        var r = new NetReader(bytes);
        return StorageMirrorUpdate.Decode(ref r);
    }

    private static byte[] Encode(StorageMirrorUpdate u)
    {
        var w = new NetWriter();
        StorageMirrorUpdate.Encode(u, w);
        return w.ToArray();
    }

    private static StorageMirrorEntry E(string key, byte slots, params StorageMirrorRow[] rows) => new(key, slots, rows);
    private static StorageMirrorRow R(byte slot, uint item, uint count, ushort level = 0) => new(slot, item, count, level, 0);

    // ---- the wire ----

    [Fact]
    public void AnUpdateRoundTripsAndABrokenOneIsDroppedWhole()
    {
        var u = new StorageMirrorUpdate(7, 3, [E("quivers:quiver", 18, R(0, 0x10, 40), R(5, 0x11, 2, 9)), E("quivers:bag", 8)]);
        var back = Decode(Encode(u));
        Assert.Equal((7u, 3u, 2), (back.View, back.Seq, back.Storages.Length));
        Assert.True(back.Storages[0].SameAs(u.Storages[0]) && back.Storages[1].SameAs(u.Storages[1]));
        Assert.True(Decode([.. Encode(u), 0, 0, 0]).Storages[0].SameAs(u.Storages[0]));  // kiln.net's padding

        var broken = new (string Why, StorageMirrorUpdate Update)[]
        {
            ("no view", u with { View = 0 }),
            ("no number", u with { Seq = 0 }),
            ("a bad key", new(7, 3, [E("Quivers:quiver", 8)])),
            ("a key without its mod", new(7, 3, [E("quiver", 8)])),
            ("a repeated key", new(7, 3, [E("a:b", 8), E("a:b", 8)])),
            ("no slots", new(7, 3, [E("a:b", 0)])),
            ("65 slots", new(7, 3, [E("a:b", 65)])),
            ("a row past the slots", new(7, 3, [E("a:b", 4, R(4, 0x10, 1))])),
            ("rows out of order", new(7, 3, [E("a:b", 8, R(3, 0x10, 1), R(2, 0x10, 1))])),
            ("a slot twice", new(7, 3, [E("a:b", 8, R(2, 0x10, 1), R(2, 0x11, 1))])),
            ("item 0", new(7, 3, [E("a:b", 8, R(0, 0, 1))])),
            ("a count of 0", new(7, 3, [E("a:b", 8, R(0, 0x10, 0))])),
            ("33 storages", new(7, 3, [.. Enumerable.Range(0, 33).Select(i => E($"m:s{i}", 1))])),
            ("257 rows", new(7, 3, [.. Enumerable.Range(0, 5).Select(i => E($"m:s{i}", 64, [.. Enumerable.Range(0, 64).Select(j => R((byte)j, 0x10, 1))]))])),
        };
        foreach (var (why, bad) in broken)
            Assert.True(Throws(Encode(bad)), why);
        Assert.True(Throws([.. Encode(u), 1]), "a trailing byte");
        Assert.True(Throws([.. Encode(u), 0, 0, 0, 0]), "more than kiln.net's padding");
        Assert.True(Throws(Encode(u)[..^3]), "a cut message");
    }

    private static bool Throws(byte[] bytes)
    {
        try
        {
            Decode(bytes);
            return false;
        }
        catch (NetFormatException)
        {
            return true;
        }
    }

    [Fact]
    public void APickAndAnAckCheckTheirFields()
    {
        static StorageAmmoPick Pick(byte[] b)
        {
            var r = new NetReader(b);
            return StorageAmmoPick.Decode(ref r);
        }
        static byte[] Bytes(StorageAmmoPick p)
        {
            var w = new NetWriter();
            StorageAmmoPick.Encode(p, w);
            return w.ToArray();
        }
        Assert.Equal(new StorageAmmoPick("quivers:quiver", 2, 0x10), Pick(Bytes(new StorageAmmoPick("quivers:quiver", 2, 0x10))));
        Assert.Throws<NetFormatException>(() => Pick(Bytes(new StorageAmmoPick("quivers:quiver", 3, 0x10))));
        Assert.Throws<NetFormatException>(() => Pick(Bytes(new StorageAmmoPick("quivers:quiver", 0, 0x10))));
        Assert.Throws<NetFormatException>(() => Pick(Bytes(new StorageAmmoPick("quivers:quiver", 1, 0))));
        Assert.Throws<NetFormatException>(() => Pick(Bytes(new StorageAmmoPick("no key", 1, 0x10))));
        var w = new NetWriter();
        StorageMirrorAck.Encode(new StorageMirrorAck(9, 4), w);
        var r = new NetReader(w.WrittenSpan);
        Assert.Equal(new StorageMirrorAck(9, 4), StorageMirrorAck.Decode(ref r));
    }

    // ---- the game's copy ----

    [Fact]
    public void TheCopyTakesOnlyNewerUpdatesOfItsViewAndSaysWhichStoragesChanged()
    {
        var store = new StorageMirrorStore();
        var first = store.Apply(new StorageMirrorUpdate(5, 1, [E("q:a", 8, R(0, 0x10, 3)), E("q:b", 8)]))!;
        Assert.Equal(["q:a", "q:b"], first.Select(c => c.Key));
        Assert.Equal([new PlayerStorageRow("q:a", 0, new ItemId(0x10), 3, 0, 0)], store.Rows("q:a"));
        Assert.Empty(store.Rows("q:b"));
        Assert.Null(store.Apply(new StorageMirrorUpdate(5, 1, [])));  // a repeat
        var second = store.Apply(new StorageMirrorUpdate(5, 3, [E("q:a", 8, R(0, 0x10, 2))]))!;
        Assert.Equal([("q:a", 1), ("q:b", 0)], second.Select(c => (c.Key, c.Rows.Length)));  // a changed, b gone
        Assert.Null(store.Apply(new StorageMirrorUpdate(5, 2, [])));  // late: older than the one applied
        Assert.NotNull(store.Apply(new StorageMirrorUpdate(6, 1, [E("q:a", 8, R(0, 0x10, 2))])));  // a new view starts over
        Assert.Equal(["q:a"], store.Keys());
        Assert.Equal(["q:a"], store.Clear().Select(c => c.Key));
        Assert.Empty(store.Keys());
    }

    // ---- the server ----

    [Fact]
    public void EachPlayerIsSentOnlyTheirOwnRowsAndOnlyWhenTheyChange()
    {
        using var r = new Run();
        var q = r.Register("quiver", 18, StorageFilter.Ammunition(AmmoKinds.Arrow | AmmoKinds.Spell));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.PlayerStorage.JoinAndRestore(r.Bo);
        r.Kiln.Step();
        r.Link.See(r.Ada);
        r.Link.See(r.Bo);
        r.Give(q, r.Ada, r.Arrow, 40);
        r.Give(q, r.Ada, r.Spell, 3);
        r.Pass();
        Assert.Equal([new PlayerStorageRow("quivers:quiver", 0, r.Arrow, 40, 0, 0), new PlayerStorageRow("quivers:quiver", 1, r.Spell, 3, 0, 0)],
            r.Link.Games[r.Ada.Entity.Value].Rows("quivers:quiver"));
        Assert.Empty(r.Link.Games[r.Bo.Entity.Value].Rows("quivers:quiver"));  // Bo has his own, empty storage
        Assert.Equal(["quivers:quiver"], r.Link.Games[r.Bo.Entity.Value].Keys());
        Assert.DoesNotContain(r.Link.Sent[r.Bo.Entity.Value], u => u.Storages.Any(s => s.Rows.Length > 0));  // never Ada's rows

        var adaView = r.Link.Sent[r.Ada.Entity.Value][0].View;
        Assert.NotEqual(adaView, r.Link.Sent[r.Bo.Entity.Value][0].View);
        r.Kiln.Run(() => PlayerStorageMirror.OnAck(r.Ada.Entity, new StorageMirrorAck(adaView, 1)));
        r.Pass();
        r.Pass(100);  // within the pass interval: no pass at all
        Assert.Single(r.Link.Sent[r.Ada.Entity.Value]);  // nothing changed

        r.Give(q, r.Ada, r.FireArrow, 5);
        r.Pass();
        Assert.Equal(2, r.Link.Sent[r.Ada.Entity.Value].Count);
        Assert.Equal(3, r.Link.Games[r.Ada.Entity.Value].Rows("quivers:quiver").Count);
        Assert.Equal(2u, r.Link.Sent[r.Ada.Entity.Value][1].Seq);
    }

    [Fact]
    public void AnUpdateTheGameDoesNotAcknowledgeIsSentAgainLaterEachTime()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8);
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Link.See(r.Ada);
        r.Give(q, r.Ada, r.Arrow, 10);
        r.Pass();
        var sent = r.Link.Sent[r.Ada.Entity.Value];
        Assert.Single(sent);
        r.Pass(4000);
        Assert.Single(sent);  // 4.3 s: still waiting
        r.Pass(1000);
        Assert.Equal(2, sent.Count);  // 5 s without an ack: again
        r.Pass(9000);
        Assert.Equal(2, sent.Count);  // the next wait is 10 s
        r.Pass(1100);
        Assert.Equal(3, sent.Count);
        r.Kiln.Run(() => PlayerStorageMirror.OnAck(r.Ada.Entity, new StorageMirrorAck(sent[0].View, sent[1].Seq)));  // an old one: ignored
        r.Pass(20_100);
        Assert.Equal(4, sent.Count);
        r.Kiln.Run(() => PlayerStorageMirror.OnAck(r.Ada.Entity, new StorageMirrorAck(sent[0].View, sent[3].Seq)));
        r.Pass(120_000);
        Assert.Equal(4, sent.Count);  // acknowledged: quiet
        Assert.Equal(1, r.Link.Games[r.Ada.Entity.Value].Rows("quivers:quiver").Count);
    }

    [Fact]
    public void AGameWithoutTheChannelIsSentNothingUntilItHasIt()
    {
        using var r = new Run();
        r.Register("quiver", 8);
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Link.See(r.Ada);
        r.Link.NoChannel.Add(r.Ada.Entity.Value);
        r.Pass();
        r.Pass();
        Assert.Empty(r.Link.Sent[r.Ada.Entity.Value]);
        r.Link.NoChannel.Clear();
        r.Pass();
        Assert.Single(r.Link.Sent[r.Ada.Entity.Value]);
    }

    [Fact]
    public void AHostingGameFillsItsOwnPlayersCopyDirectlyAndEmptiesItWhenThePlayerGoes()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8);
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Link.See(r.Ada, local: true);
        r.Give(q, r.Ada, r.Arrow, 12);
        r.Pass();
        Assert.Empty(r.Link.Sent[r.Ada.Entity.Value]);  // nothing over the network
        Assert.True(r.Kiln.Run(() => PlayerStorage.Client.Available));
        Assert.Equal([new PlayerStorageRow("quivers:quiver", 0, r.Arrow, 12, 0, 0)], r.Kiln.Run(() => PlayerStorage.Client.Rows("quivers:quiver")));
        r.Kiln.Step();
        var changed = Assert.Single(r.Changes);
        Assert.Equal(("quivers:quiver", 1), (changed.Key, changed.Rows.Count));
        r.Link.Now.Clear();  // the player left
        r.Pass();
        r.Kiln.Step();
        Assert.Empty(r.Kiln.Run(() => PlayerStorage.Client.Rows("quivers:quiver")));
        Assert.Equal(2, r.Changes.Count);
        Assert.Empty(r.Changes[1].Rows);
    }

    [Fact]
    public void WhenAHostedWorldStopsItsCopyEmptiesAndNothingOfItIsHandledInTheNextWorld()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8, StorageFilter.Ammunition(AmmoKinds.Arrow));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.PlayerStorage.JoinAndRestore(r.Bo);
        r.Kiln.Step();
        r.Link.Hosting = true;
        r.Link.LocalPlayer = r.Ada.Entity;
        r.Link.See(r.Ada, local: true);
        r.Link.See(r.Bo);
        r.Give(q, r.Ada, r.Arrow, 12);
        r.Pass();
        Assert.Single(r.Kiln.Run(() => PlayerStorage.Client.Rows("quivers:quiver")));
        var boView = Assert.Single(r.Link.Sent[r.Bo.Entity.Value]).View;
        var equips = new Equips();
        PlayerStorageMirror.EquipHook = equips.Equip;
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow)));
        Assert.True(PlayerStorageMirror.Pick("quivers:quiver", AmmoKinds.Arrow, r.Arrow));  // the HUD: queued for the server tick

        // the game's menu runs Kiln's service tick off the tick thread: a queued pick waits for a world, it is not checked there
        PlayerStorageMirror.Pass();
        Assert.Empty(equips.Calls);

        // the hosted world stops (ServerStoppingEvent): the copy empties at once, and the queued pick goes with the world
        r.Kiln.Run(PlayerStorageMirror.WorldStopping);
        Assert.Empty(r.Kiln.Run(() => PlayerStorage.Client.Rows("quivers:quiver")));
        r.Kiln.Step();
        Assert.Empty(r.Changes[^1].Rows);
        r.Pass();
        Assert.Empty(equips.Calls);
        Assert.NotEqual(boView, r.Link.Sent[r.Bo.Entity.Value][^1].View);  // the next world: a new view for every player
    }

    [Fact]
    public void WithoutKilnsNetworkTheCopyIsOffAndSaysWhy()
    {
        using var r = new Run();
        r.Kiln.Run(() => PlayerStorageMirror.UseLinkForTests(null));
        var row = r.Kiln.Run(() => Capabilities.Get(PlayerStorage.Client.Capability));
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.ServiceMissing), (row.State, row.Reason));
        Assert.Contains("kiln.net", row.ReasonText);
        Assert.Equal("items.storage.client", Assert.Throws<KilnUnavailableException>(() => r.Kiln.Run(() => PlayerStorage.Client.Rows("quivers:quiver"))).Capability);
        r.Kiln.Run(() => PlayerStorageMirror.UseLinkForTests(r.Link));
        Assert.True(r.Kiln.Run(() => PlayerStorage.Client.Available));
        Assert.Empty(r.Kiln.Run(() => PlayerStorage.Client.Rows("quivers:quiver")));
    }

    // ---- ProvideAmmo: the server half ----

    private sealed class Equips
    {
        public readonly List<(EntityId Player, SlotId Slot, EquipmentSlot To)> Calls = [];
        public TaskCompletionSource<EquipResult>? Hold;

        public Task<EquipResult> Equip(EntityId player, SlotId slot, EquipmentSlot to, EntityId entity)
        {
            Calls.Add((player, slot, to));
            return Hold?.Task ?? Task.FromResult(EquipResult.Fail(ItemsStatus.Done, player, to, false, slot, "", false));
        }
    }

    [Fact]
    public void APickIsAWishTheServerChecksBeforeItEquipsTheStorageSlot()
    {
        using var r = new Run();
        var q = r.Register("quiver", 18, StorageFilter.Ammunition(AmmoKinds.Arrow | AmmoKinds.Spell));
        var bag = r.Register("bag", 8);
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.PlayerStorage.JoinAndRestore(r.Bo);
        r.Kiln.Step();
        r.Link.See(r.Ada);
        r.Link.See(r.Bo);
        r.Give(q, r.Ada, r.Spell, 2);
        r.Give(q, r.Ada, r.Arrow, 30);
        r.Give(bag, r.Ada, r.Arrow, 7);
        var equips = new Equips();
        PlayerStorageMirror.EquipHook = equips.Equip;
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow)));
        void Pick(FakePlayer p, string key, AmmoKinds kind, ItemId item) =>
            r.Kiln.Run(() => PlayerStorageMirror.HandlePick(p.Entity, new StorageAmmoPick(key, (byte)kind, item.Value)));

        Pick(r.Ada, "quivers:quiver", AmmoKinds.Arrow, r.Arrow);
        var call = Assert.Single(equips.Calls);
        Assert.Equal((r.Ada.Entity, r.Kiln.Run(() => q.Slot(r.Ada.Entity, 1)), EquipmentSlot.Arrow), call);  // slot 1 holds the arrows
        Pick(r.Ada, "quivers:bag", AmmoKinds.Arrow, r.Arrow);        // its mod provides no ammunition of the bag
        Pick(r.Ada, "quivers:quiver", AmmoKinds.Spell, r.Spell);     // not a kind it provides
        Pick(r.Ada, "quivers:quiver", AmmoKinds.Arrow, r.Spell);     // a spell picked as an arrow
        Pick(r.Ada, "quivers:quiver", AmmoKinds.Arrow, r.FireArrow); // not in the storage
        Pick(r.Bo, "quivers:quiver", AmmoKinds.Arrow, r.Arrow);      // Bo's own storage has none: never Ada's
        Pick(r.Ada, "others:quiver", AmmoKinds.Arrow, r.Arrow);      // no such storage
        Assert.Single(equips.Calls);
        Assert.Contains(r.Kiln.Logs, l => l.Text.Contains("was not taken"));
    }

    [Fact]
    public void PicksAreLimitedPerPlayerAndARepeatWhileOneIsUnderWayIsIgnored()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8, StorageFilter.Ammunition(AmmoKinds.Arrow));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Link.See(r.Ada);
        r.Give(q, r.Ada, r.Arrow, 30);
        var equips = new Equips { Hold = new TaskCompletionSource<EquipResult>() };
        PlayerStorageMirror.EquipHook = equips.Equip;
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow)));
        var pick = new StorageAmmoPick("quivers:quiver", 1, r.Arrow.Value);
        r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, pick));
        r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, pick));
        Assert.Single(equips.Calls);  // the first is under way
        r.Kiln.Run(() => equips.Hold.SetResult(EquipResult.Fail(ItemsStatus.Done, r.Ada.Entity, EquipmentSlot.Arrow, false, default, "", false)));
        equips.Hold = null;
        for (var i = 0; i < 12; i++) r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, pick));
        Assert.Equal(7, equips.Calls.Count);  // a burst of 8 a player: 2 went above, 6 of these 12
        r.Now += 1000;
        for (var i = 0; i < 12; i++) r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, pick));
        Assert.Equal(11, equips.Calls.Count);  // 4 a second after that
    }

    [Fact]
    public void ProvideAmmoIsForTheModsOwnStoragesOnePerStorage()
    {
        using var r = new Run();
        var other = Mod("others");
        Assert.Throws<ArgumentException>(() => r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("others:quiver", AmmoKinds.Arrow))));
        Assert.Throws<ArgumentException>(() => r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quiver", AmmoKinds.Arrow))));
        Assert.Throws<ArgumentException>(() => r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", 0))));
        Assert.Throws<InvalidOperationException>(() => r.Kiln.Run(() => As(null, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow))));
        var first = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow)));
        Assert.Throws<InvalidOperationException>(() => r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Spell))));
        first.Dispose();
        Assert.False(first.Active);
        using var again = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Spell)));
        Assert.Equal(("quivers:quiver", AmmoKinds.Spell), (again.FullKey, again.Kinds));
        using var theirs = r.Kiln.Run(() => As(other, () => PlayerStorage.Client.ProvideAmmo("others:quiver", AmmoKinds.Arrow)));
        Assert.True(theirs.Active);
    }

    // ---- ProvideAmmo: the HUD half (the kit's fake kiln.ui.ammo) ----

    [Fact]
    public void TheHudListsShowTheStoragesAmmunitionOneRowPerItemAndAPickGoesToTheServer()
    {
        using var r = new Run();
        var bow = r.Kiln.Items.AddItem("Weapon_Bow_T1", 1, ItemCategory.Weapons, gear: true);
        var staff = r.Kiln.Items.AddItem("Weapon_Staff_T1", 1, ItemCategory.Weapons, gear: true);
        PlayerStorageMirror.Store.Apply(new StorageMirrorUpdate(4, 1,
            [E("quivers:quiver", 18, R(0, r.Arrow.Value, 30), R(1, r.Spell.Value, 2), R(2, r.FireArrow.Value, 5), R(3, r.Arrow.Value, 9), R(4, r.Logs.Value, 50))]));
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow | AmmoKinds.Spell)));
        Assert.True(ammo.Shown, ammo.WhyNotShown);
        r.Kiln.AmmoList.Hold(AmmoListKind.Arrow, bow);
        r.Kiln.AmmoList.Hold(AmmoListKind.Spell, staff);
        r.Kiln.Step(2);
        Assert.Equal([new AmmoListRow(r.Arrow, 39), new AmmoListRow(r.FireArrow, 5)], r.Kiln.AmmoList.Provided(AmmoListKind.Arrow));
        Assert.Equal([new AmmoListRow(r.Spell, 2)], r.Kiln.AmmoList.Provided(AmmoListKind.Spell));
        Assert.Equal(AmmoListMode.Append, r.Kiln.AmmoList.ModeOf(AmmoListKind.Arrow));

        // the copy changes: the rows follow
        r.Kiln.Run(() => PlayerStorageMirror.Store.Apply(new StorageMirrorUpdate(4, 2, [E("quivers:quiver", 18, R(2, r.FireArrow.Value, 4))])));
        r.Kiln.Run(() => PlayerStorageAmmo.Changed("quivers:quiver"));
        r.Kiln.Step(2);
        Assert.Equal([new AmmoListRow(r.FireArrow, 4)], r.Kiln.AmmoList.Provided(AmmoListKind.Arrow));

        // a pick of the row goes to the server as a wish
        Assert.True(r.Kiln.AmmoList.Pick(AmmoListKind.Arrow, r.FireArrow));
        r.Kiln.Step(2);
        Assert.Equal([new StorageAmmoPick("quivers:quiver", 1, r.FireArrow.Value)], r.Link.Picks);

        // a hosting game hands the pick to its own server tick
        r.Link.Hosting = true;
        r.Link.LocalPlayer = r.Ada.Entity;
        var equips = new Equips();
        PlayerStorageMirror.EquipHook = equips.Equip;
        Assert.True(r.Kiln.AmmoList.Pick(AmmoListKind.Arrow, r.FireArrow));
        r.Kiln.Step(2);
        Assert.Single(r.Link.Picks);
        r.Pass();  // the queue is the server's: the pick is checked there (Ada has no such storage here: refused)
        Assert.Empty(equips.Calls);
        Assert.Contains(r.Kiln.Logs, l => l.Text.Contains("pick of quivers:quiver") && l.Text.Contains("was not taken"));

        ammo.Dispose();
        r.Kiln.Step(2);
        Assert.Empty(r.Kiln.AmmoList.Provided(AmmoListKind.Arrow));
    }

    [Fact]
    public void TheServerMarksArrowsAndSpellsAndAGameWithoutAnItemCatalogListsThemByTheMarks()
    {
        using var r = new Run();
        var bag = r.Register("bag", 8);
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Link.See(r.Ada);
        r.Give(bag, r.Ada, r.Arrow, 10);
        r.Give(bag, r.Ada, r.Spell, 2);
        r.Give(bag, r.Ada, r.Logs, 5);
        r.Pass();
        // the server's catalog says what each item is: Flags carries its AmmoKinds (1 arrows, 2 a spell, 0 anything else)
        Assert.Equal([1u, 2u, 0u], r.Link.Games[r.Ada.Entity.Value].Rows("quivers:bag").Select(x => x.Flags));

        // A game that joined another's world has no item catalog (Items.Get answers null there): only the server's marks
        // say which rows are arrows and spells.
        ItemId arrow = new(0x5A0001), spell = new(0x5A0002), other = new(0x5A0003);
        Assert.Null(r.Kiln.Run(() => Items.Get(arrow)));
        PlayerStorageMirror.Store.Apply(new StorageMirrorUpdate(9, 1,
            [E("quivers:quiver", 8, new StorageMirrorRow(0, arrow.Value, 20, 0, 1), new StorageMirrorRow(1, spell.Value, 3, 0, 2),
                new StorageMirrorRow(2, other.Value, 7, 0, 0))]));
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow | AmmoKinds.Spell)));
        r.Kiln.AmmoList.Hold(AmmoListKind.Arrow, r.Kiln.Items.AddItem("Weapon_Bow_T1", 1, ItemCategory.Weapons, gear: true));
        r.Kiln.AmmoList.Hold(AmmoListKind.Spell, r.Kiln.Items.AddItem("Weapon_Staff_T1", 1, ItemCategory.Weapons, gear: true));
        r.Kiln.Step(2);
        Assert.Equal([new AmmoListRow(arrow, 20)], r.Kiln.AmmoList.Provided(AmmoListKind.Arrow));
        Assert.Equal([new AmmoListRow(spell, 3)], r.Kiln.AmmoList.Provided(AmmoListKind.Spell));
        Assert.True(r.Kiln.AmmoList.Pick(AmmoListKind.Spell, spell));
        r.Kiln.Step(2);
        Assert.Equal([new StorageAmmoPick("quivers:quiver", 2, spell.Value)], r.Link.Picks);
    }

    [Fact]
    public void ReplaceShowsOnlyTheStoragesRows()
    {
        using var r = new Run();
        var bow = r.Kiln.Items.AddItem("Weapon_Bow_T1", 1, ItemCategory.Weapons, gear: true);
        PlayerStorageMirror.Store.Apply(new StorageMirrorUpdate(4, 1, [E("quivers:quiver", 8, R(0, r.FireArrow.Value, 5))]));
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () =>
            PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow, new PlayerStorageAmmoOptions { Replace = true })));
        r.Kiln.AmmoList.Hold(AmmoListKind.Arrow, bow, new AmmoListRow(r.Arrow, 20));
        r.Kiln.Step(2);
        Assert.Equal(AmmoListMode.Replace, r.Kiln.AmmoList.ModeOf(AmmoListKind.Arrow));
        Assert.Equal([new AmmoListRow(r.FireArrow, 5)], r.Kiln.AmmoList.Shown(AmmoListKind.Arrow));
    }

    // ---- Activate ----

    [Fact]
    public void ActivateRefusesWhatItCannotSwapIn()
    {
        using var r = new Run();
        var bag = r.Register("bag", 8);
        r.Kiln.Step();
        var early = r.Kiln.Run(() => bag.Activate(r.Ada.Entity, 0));
        Assert.Equal(ItemsStatus.NoInventory, early.Result.Status);
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        Assert.Equal(ItemsStatus.SlotChanged, r.Kiln.Run(() => bag.Activate(r.Ada.Entity, 0)).Result.Status);
        r.Give(bag, r.Ada, r.Logs, 10);
        var logs = r.Kiln.Run(() => bag.Activate(r.Ada.Entity, 0)).Result;
        Assert.Equal(ItemsStatus.Refused, logs.Status);
        Assert.Contains("no arrow or spell", logs.Message);
        r.Give(bag, r.Ada, r.Arrow, 10);
        // the kit has no moves (items.move): the swap is handed to Items.Move, which says so
        Assert.Equal("items.move", Assert.Throws<KilnUnavailableException>(() => r.Kiln.Run(() => { _ = bag.Activate(r.Ada.Entity, 1); })).Capability);
        Assert.Throws<ArgumentOutOfRangeException>(() => r.Kiln.Run(() => { _ = bag.Activate(r.Ada.Entity, 8); }));
    }

    /// <summary>Stands in for kiln.items' move and equip in Activate (the kit has neither). Each request waits until the
    /// test completes it, and completes as kiln.items' do: their continuations never run inline.</summary>
    private sealed class Steps
    {
        public readonly List<(EntityId From, SlotId Slot, EntityId To, int Count, MoveOptions Options, bool Tick)> Moves = [];
        public readonly List<(EntityId Player, SlotId From, EquipmentSlot Slot, bool Tick)> Equips = [];
        public readonly Queue<TaskCompletionSource<MoveResult>> MoveWaits = new();
        public readonly Queue<TaskCompletionSource<EquipResult>> EquipWaits = new();

        public Steps()
        {
            PlayerStorageActivation.MoveHook = (from, slot, to, count, options) =>
            {
                var wait = new TaskCompletionSource<MoveResult>(TaskCreationOptions.RunContinuationsAsynchronously);
                lock (this)
                {
                    Moves.Add((from, slot, to, count, options, Game.IsTickThread));
                    MoveWaits.Enqueue(wait);
                }
                return wait.Task;
            };
            PlayerStorageActivation.EquipHook = (player, from, slot) =>
            {
                var wait = new TaskCompletionSource<EquipResult>(TaskCreationOptions.RunContinuationsAsynchronously);
                lock (this)
                {
                    Equips.Add((player, from, slot, Game.IsTickThread));
                    EquipWaits.Enqueue(wait);
                }
                return wait.Task;
            };
        }

        public int MoveCount
        {
            get { lock (this) return Moves.Count; }
        }

        public int EquipCount
        {
            get { lock (this) return Equips.Count; }
        }
    }

    private static MoveResult Moved(EntityId from, SlotId fromSlot, EntityId to, SlotId toSlot, ItemId item, int n) =>
        new(ItemsStatus.Done, MoveKind.Stack, from, to, n, n, 1, [new MoveChange(fromSlot, toSlot, item, n, default)], "", 0, false, 1, false);

    private static EquipResult Equipped(EntityId player, SlotId cell, ItemId item) =>
        new(ItemsStatus.Done, player, EquipmentSlot.Arrow, false, item, default, cell, cell, default, default, default, "", 0, 1, false);

    // What the mod's scheduler holds, run on the tick thread as Kiln.Host runs it at the start of the mod's Pre.
    private static void Drain(Run r, ModContext mod) => r.Kiln.Run(() =>
    {
        var sync = SynchronizationContext.Current;
        var outer = HostAccess.Current;
        SynchronizationContext.SetSynchronizationContext(mod.Scheduler.SynchronizationContext);
        HostAccess.Current = mod;
        try
        {
            while (true)
            {
                Action? next;
                lock (mod.PendingLock)
                    if (!mod.Pending.TryDequeue(out next)) break;
                next();
            }
        }
        finally
        {
            HostAccess.Current = outer;
            SynchronizationContext.SetSynchronizationContext(sync);
        }
    });

    private static void DrainUntil(Run r, ModContext mod, Func<bool> done)
    {
        var until = Environment.TickCount64 + 5000;
        do
        {
            Drain(r, mod);
            if (done()) return;
            Thread.Sleep(5);
        } while (Environment.TickCount64 < until);
    }

    // a slot of the player's backpack holding count of item (the kit's inventories: what a move there would leave)
    private static SlotId BackpackStack(Run r, FakePlayer p, ItemId item, int count)
    {
        var give = r.Kiln.Run(() => Items.Give(p.Entity, item, count, null, GiveOptions.NoMerge));
        r.Kiln.Step();
        Assert.Equal(ItemsStatus.Done, give.Result.Status);
        return r.Kiln.Run(() => Items.Read(p.Entity)!.Slots.Last(s => s.Item == item).Slot);
    }

    [Fact]
    public void APickThatActivatesTakesEachStepOnTheTickThreadAsTheStoragesMod()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8, StorageFilter.Ammunition(AmmoKinds.Arrow));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Give(q, r.Ada, r.Arrow, 30);
        var holder = r.Holder(q, r.Ada);
        var cell = BackpackStack(r, r.Ada, r.Arrow, 30);  // where the move puts the stack
        var steps = new Steps();
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () =>
            PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow, new PlayerStorageAmmoOptions { Activate = true })));
        var pick = new StorageAmmoPick("quivers:quiver", 1, r.Arrow.Value);

        r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, pick));
        var move = Assert.Single(steps.Moves);
        var from = r.Kiln.Run(() => q.Slot(r.Ada.Entity, 0));
        Assert.Equal((holder, from, r.Ada.Entity, true), (move.From, move.Slot, move.To, move.Tick));
        r.Kiln.Run(() => steps.MoveWaits.Dequeue().SetResult(Moved(holder, from, r.Ada.Entity, cell, r.Arrow, 30)));
        DrainUntil(r, r.Quivers, () => steps.EquipCount > 0);
        // kiln.items completes off the step: the equip still runs on the tick thread (the storage's mod's scheduler)
        Assert.Equal((r.Ada.Entity, cell, EquipmentSlot.Arrow, true), Assert.Single(steps.Equips));
        r.Kiln.Run(() => steps.EquipWaits.Dequeue().SetResult(Equipped(r.Ada.Entity, cell, r.Arrow)));
        DrainUntil(r, r.Quivers, () => r.Kiln.Logs.Any(l => l.Text.Contains("(activate): equip Done")));
        Assert.Contains(r.Kiln.Logs, l => l.Text.Contains("pick by") && l.Text.Contains("(activate): equip Done"));

        // finished: the same pick is no repeat of one under way any more
        r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, pick));
        Assert.Equal(2, steps.MoveCount);
    }

    [Fact]
    public void ADirectPicksOutcomeIsHandledOnTheTickThread()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8, StorageFilter.Ammunition(AmmoKinds.Arrow));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Give(q, r.Ada, r.Arrow, 30);
        var wait = new TaskCompletionSource<EquipResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        var calls = 0;
        PlayerStorageMirror.EquipHook = (_, _, _, _) => ++calls == 1 ? wait.Task : Task.FromResult(Equipped(r.Ada.Entity, default, r.Arrow));
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow)));
        var pick = new StorageAmmoPick("quivers:quiver", 1, r.Arrow.Value);
        r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, pick));
        r.Kiln.Run(() => wait.SetResult(Equipped(r.Ada.Entity, default, r.Arrow)));
        Thread.Sleep(200);
        // kiln.items completed the equip off the step: its outcome waits for the tick thread
        Assert.DoesNotContain(r.Kiln.Logs, l => l.Text.Contains("pick by") && l.Text.Contains("equip Done"));
        Drain(r, r.Quivers);
        Assert.Contains(r.Kiln.Logs, l => l.Text.Contains("pick by") && l.Text.Contains("equip Done"));
        r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, pick));
        Assert.Equal(2, calls);
    }

    [Fact]
    public void AnActivateWhoseEquipFailsPutsWhatMovedBackAndOnlyOneRunsAtATime()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8, StorageFilter.Ammunition(AmmoKinds.Arrow));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Give(q, r.Ada, r.Arrow, 30);
        r.Give(q, r.Ada, r.FireArrow, 6);
        var holder = r.Holder(q, r.Ada);
        var cell = BackpackStack(r, r.Ada, r.Arrow, 30);
        var steps = new Steps();
        var from = r.Kiln.Run(() => q.Slot(r.Ada.Entity, 0));

        var first = r.Kiln.Run(() => As(r.Quivers, () => q.Activate(r.Ada.Entity, 0)));
        Assert.Single(steps.Moves);
        // another activation of the same player's arrows while the first runs: refused, nothing moves
        var second = r.Kiln.Run(() => As(r.Quivers, () => q.Activate(r.Ada.Entity, 1)));
        Assert.True(second.IsCompleted);
        Assert.Equal(ItemsStatus.Refused, second.Result.Status);
        Assert.Single(steps.Moves);

        // the stack reached the backpack but the game refuses the equip: what moved goes back into the storage
        r.Kiln.Run(() => steps.MoveWaits.Dequeue().SetResult(Moved(holder, from, r.Ada.Entity, cell, r.Arrow, 30)));
        DrainUntil(r, r.Quivers, () => steps.EquipCount > 0);
        r.Kiln.Run(() => steps.EquipWaits.Dequeue().SetResult(
            EquipResult.Fail(ItemsStatus.Refused, r.Ada.Entity, EquipmentSlot.Arrow, false, cell, "the game refused it", false)));
        DrainUntil(r, r.Quivers, () => steps.MoveCount > 1);
        var back = steps.Moves[1];
        Assert.Equal((r.Ada.Entity, cell, holder, 30, true), (back.From, back.Slot, back.To, back.Count, back.Tick));
        r.Kiln.Run(() => steps.MoveWaits.Dequeue().SetResult(Moved(r.Ada.Entity, cell, holder, from, r.Arrow, 30)));
        DrainUntil(r, r.Quivers, () => first.IsCompleted);
        Assert.Equal(ItemsStatus.Refused, first.Result.Status);
        Assert.Contains("the game refused it", first.Result.Message);

        // over: the next activation runs
        _ = r.Kiln.Run(() => As(r.Quivers, () => q.Activate(r.Ada.Entity, 1)));
        Assert.Equal(3, steps.MoveCount);
    }

    [Fact]
    public void RefusalsAreLoggedOncePerReasonAndAnIdlePlayersPickRateIsForgotten()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8, StorageFilter.Ammunition(AmmoKinds.Arrow));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        r.Link.See(r.Ada);
        r.Give(q, r.Ada, r.Arrow, 30);
        using var ammo = r.Kiln.Run(() => As(r.Quivers, () => PlayerStorage.Client.ProvideAmmo("quivers:quiver", AmmoKinds.Arrow)));
        // six made-up items: one line for the reason, not one per item a game can make up
        for (var i = 0; i < 6; i++)
            r.Kiln.Run(() => PlayerStorageMirror.HandlePick(r.Ada.Entity, new StorageAmmoPick("quivers:quiver", 1, 0x7000u + (uint)i)));
        Assert.Single(r.Kiln.Logs, l => l.Text.Contains("was not taken"));
        Assert.Equal(1, PlayerStorageMirror.PickStateCount);
        r.Pass(2500);  // the bucket is full again: nothing is kept for the player
        Assert.Equal(0, PlayerStorageMirror.PickStateCount);
    }
}
