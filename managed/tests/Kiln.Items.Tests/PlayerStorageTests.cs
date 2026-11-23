// SPDX-License-Identifier: MIT
// Player storage (0.2.0-dev.30.7, GitHub issue 128): Items.RegisterPlayerStorage, PlayerStorage, StorageFilter,
// Items.PlayerStorages and the storage events on the test kit's kiln.items.storage (Fakes/ItemsStorage: holders are fake
// containers, the game's side is scripted with KilnHarness.PlayerStorage). What the game does (the capture, the join, the
// tombstone) needs native implementation and the owner's in-game verification; persistence is Kiln.Saves'.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed class PlayerStorageTests
{
    private const ulong AdaSteam = 76561198000000128;

    private static ModContext Mod(string id) =>
        HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", Path.GetTempPath()), Backend.Current.OwnerCreate("mod:" + id), Path.GetTempPath());

    private static T As<T>(ModContext mod, Func<T> f)
    {
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try { return f(); }
        finally { HostAccess.Current = previous; }
    }

    private sealed class Run : IDisposable
    {
        public readonly KilnHarness Kiln = new();
        public readonly FakePlayer Ada;
        public readonly ModContext Quivers;
        public readonly ItemId Arrow, Bolt, Logs, Sword;
        public readonly List<KilnEvent> Seen = [];
        private readonly List<IDisposable> _subs = [];

        public Run(bool catalog = true)
        {
            if (catalog)
            {
                Arrow = Kiln.Items.AddItem("Ammo_Arrow_T1", 99, ItemCategory.Ammunition);
                Kiln.Items.SetEquipmentSlotType(Arrow, EquipmentSlot.Arrow);
                Bolt = Kiln.Items.AddItem("Ammo_Bolt_T1", 99, ItemCategory.Ammunition);
                Logs = Kiln.Items.AddItem("Material_T1_Wood_Logs", 50, ItemCategory.Materials);
                Sword = Kiln.Items.AddItem("Weapon_Sword_T1", 1, ItemCategory.Weapons, gear: true, levelMin: 1, levelMax: 5);
            }
            Ada = Kiln.Players.Join("Ada", AdaSteam, position: new WorldPosition(0, 0, 0));
            Kiln.Step(2);
            Quivers = Mod("quivers");
            Kiln.Run(() =>
            {
                _subs.Add(Events.On<PlayerStorageRestoredEvent>(e => Seen.Add(e)));
                _subs.Add(Events.On<PlayerStorageParkedEvent>(e => Seen.Add(e)));
                _subs.Add(Events.On<PlayerStorageDeathEvent>(e => Seen.Add(e)));
            });
        }

        public PlayerStorage Register(string key, int slots, StorageFilter filter = default, StorageDeath death = StorageDeath.Keep, ModContext? mod = null) =>
            Kiln.Run(() => As(mod ?? Quivers, () => Items.RegisterPlayerStorage(key, slots, filter, death)));

        public void Dispose()
        {
            Kiln.Run(() => { foreach (var s in _subs) s.Dispose(); });
            Kiln.Dispose();
        }
    }

    // ---- registration ----

    [Fact]
    public void ARegistrationIsKeyedByTheModAndChecksItsArguments()
    {
        using var r = new Run();
        var q = r.Register("quiver", 18, StorageFilter.Ammunition(AmmoKinds.Arrow), StorageDeath.FollowGame);
        Assert.Equal(("quiver", "quivers:quiver", 18, StorageDeath.FollowGame), (q.Key, q.FullKey, q.Slots, q.Death));
        foreach (var bad in new[] { "", "Quiver", "a:b", "-x", new string('a', 33), "has space" })
            Assert.Throws<ArgumentException>(() => r.Register(bad, 8));
        Assert.Throws<ArgumentOutOfRangeException>(() => r.Register("zero", 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => r.Register("big", 65));
        Assert.Throws<ArgumentException>(() => r.Register("death", 8, death: (StorageDeath)7));
        var again = Assert.Throws<InvalidOperationException>(() => r.Register("quiver", 8));
        Assert.Contains("quivers:quiver", again.Message);
        Assert.Throws<InvalidOperationException>(() => r.Kiln.Run(() => Items.RegisterPlayerStorage("nomod", 8, StorageFilter.Any)));
    }

    [Fact]
    public void AModHasEightStoragesAtMost()
    {
        using var r = new Run();
        for (var i = 0; i < 8; i++) r.Register($"bag{i}", 8);
        var ninth = Assert.Throws<InvalidOperationException>(() => r.Register("bag8", 8));
        Assert.Contains("8", ninth.Message);
        r.Register("bag0", 8, mod: Mod("other"));  // another mod's key of the same name is its own
    }

    [Fact]
    public void WithTheRowOffRegistrationThrowsNamingIt()
    {
        using var r = new Run();
        r.Kiln.Capabilities.Override(PlayerStorage.Capability, false, "test");
        Assert.False(r.Kiln.Run(() => PlayerStorage.Available));
        var e = Assert.Throws<KilnUnavailableException>(() => r.Register("quiver", 8));
        Assert.Equal("items.storage", e.Capability);
        Assert.Equal("items.storage", Assert.Throws<KilnUnavailableException>(() => r.Kiln.Run(() => Items.PlayerStorages.Of(r.Ada.Entity))).Capability);
    }

    // ---- restore and use ----

    [Fact]
    public void AStorageIsReadyOnceKilnRestoredItAtTheJoin()
    {
        using var r = new Run();
        var q = r.Register("quiver", 18);
        r.Kiln.Step();
        Assert.False(r.Kiln.Run(() => q.IsReady(r.Ada.Entity)));
        Assert.Equal(PlayerStorageState.Waiting, r.Kiln.Run(() => q.Status(r.Ada.Entity)).State);  // registered, not restored yet
        Assert.True(r.Kiln.Run(() => q.EntityOf(r.Ada.Entity)).IsNone);
        Assert.Null(r.Kiln.Run(() => q.Read(r.Ada.Entity)));

        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        Assert.True(r.Kiln.Run(() => q.IsReady(r.Ada.Entity)));
        var status = r.Kiln.Run(() => q.Status(r.Ada.Entity));
        Assert.Equal((PlayerStorageState.Ready, PlayerStorageSource.New, true, 0), (status.State, status.Source, status.Restored, status.Stacks));
        var holder = r.Kiln.Run(() => q.EntityOf(r.Ada.Entity));
        Assert.False(holder.IsNone);
        var contents = r.Kiln.Run(() => q.Read(r.Ada.Entity))!;
        Assert.Equal(3, contents.Inventories.Count);  // 18 slots: 3 pages
        Assert.Equal(contents.Inventories[2].Inventory, r.Kiln.Run(() => q.Slot(r.Ada.Entity, 17)).Inventory);
        Assert.Equal(1, r.Kiln.Run(() => q.Slot(r.Ada.Entity, 17)).Index);
        Assert.Throws<ArgumentOutOfRangeException>(() => r.Kiln.Run(() => q.Slot(r.Ada.Entity, 18)));
        var restored = Assert.Single(r.Seen.OfType<PlayerStorageRestoredEvent>());
        Assert.Equal((r.Ada.Entity, "quivers:quiver", 0, PlayerStorageSource.New, false), (restored.Player, restored.Key, restored.Stacks, restored.Source, restored.Live));
    }

    [Fact]
    public void ARestoredCopyComesBackWithItsStacksAndItsKeysWithoutAModWaitParked()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8);
        r.Kiln.Step();
        r.Kiln.PlayerStorage.Join(r.Ada);
        r.Kiln.PlayerStorage.Restore(r.Ada, PlayerStorageSource.Client, paired: true, revision: 41,
            new HarnessStorageCopy("quivers:quiver", 8, [new(0, r.Arrow, 64), new(3, r.Arrow, 12)]),
            new HarnessStorageCopy("oldmod:bag", 8, [new(1, r.Logs, 20)], Parked: true));
        r.Kiln.PlayerStorage.Apply(r.Ada);
        r.Kiln.Step();
        Assert.Equal(76, r.Kiln.Run(() => q.Read(r.Ada.Entity))!.Count(r.Arrow));
        var status = r.Kiln.Run(() => q.Status(r.Ada.Entity));
        Assert.Equal((true, PlayerStorageSource.Client, 2, 41L), (status.Paired, status.Source, status.Stacks, status.Revision));
        var restored = Assert.Single(r.Seen.OfType<PlayerStorageRestoredEvent>());
        Assert.Equal((2, true, PlayerStorageSource.Client), (restored.Stacks, restored.Paired, restored.Source));

        var all = r.Kiln.Run(() => Items.PlayerStorages.Of(r.Ada.Entity));
        Assert.Equal(new[] { "oldmod:bag", "quivers:quiver" }, all.Select(i => i.FullKey));
        var bag = all[0];
        Assert.Equal((false, 0, PlayerStorageState.Parked, 1), (bag.Registered, bag.Slots, bag.Status.State, bag.Status.Parked));
        Assert.True(bag.Holder.IsNone);
        Assert.True(all[1].Registered);
        var parked = Assert.Single(r.Seen.OfType<PlayerStorageParkedEvent>());
        Assert.Equal(("oldmod:bag", 1, PlayerStorageParkReason.Unregistered), (parked.Key, parked.Stacks, parked.Reason));
    }

    private static string _returningKey = "";

    [Theory]
    [InlineData("gone:bag")]
    [InlineData("quivers:quiver")]
    public unsafe void PublicEnumerationHidesTemporaryReturnHoldersButKeepsTheOrdinaryRows(string returningKey)
    {
        using var r = new Run();
        _returningKey = returningKey;
        var table = *StorageNative.T;
        table.List = &ListWithTemporaryReturnHolders;
        StorageNative.UseForTests(&table);
        try
        {
            // Saves still needs both raw rows per original key for InstanceData tracking.
            var raw = stackalloc StorageNative.State[3];
            raw[0].Size = (uint)sizeof(StorageNative.State);
            uint count;
            Assert.Equal(StorageNative.Ok, StorageNative.T->List(r.Ada.Entity.Value, raw, 3, &count));
            Assert.Equal(3u, count);
            Assert.Equal((StorageNative.StateParked, 0u, 901u), (raw[1].Status, raw[1].Handle, raw[1].Holder));

            var all = r.Kiln.Run(() => Items.PlayerStorages.Of(r.Ada.Entity));
            Assert.Equal(new[] { "gone:bag", "quivers:quiver" }, all.Select(i => i.FullKey));
            Assert.Equal((false, EntityId.None, PlayerStorageState.Parked), (all[0].Registered, all[0].Holder, all[0].Status.State));
            Assert.Equal((true, new EntityId(800), PlayerStorageState.Ready), (all[1].Registered, all[1].Holder, all[1].Status.State));
        }
        finally { StorageNative.ResetForTests(); }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int ListWithTemporaryReturnHolders(uint player, StorageNative.State* rows, uint capacity, uint* count)
    {
        *count = 3;
        if (capacity < 3) return StorageNative.BufferTooSmall;
        for (var i = 0; i < 3; i++)
        {
            rows[i] = default;
            rows[i].Size = (uint)sizeof(StorageNative.State);
            StorageNative.SetText(rows[i].Key, StorageNative.KeyMax, i == 0 ? "gone:bag" : i == 1 ? _returningKey : "quivers:quiver");
            rows[i].Status = i == 2 ? StorageNative.StateReady : StorageNative.StateParked;
            rows[i].Handle = i == 2 ? 1u : 0;
            rows[i].Holder = i == 2 ? 800u : i % 2 == 1 ? (uint)(900 + i) : 0;
            rows[i].Slots = i % 2 == 1 ? 64u : i == 2 ? 8u : 0;
            rows[i].Pages = i % 2 == 1 ? 8u : i == 2 ? 1u : 0;
        }
        return StorageNative.Ok;
    }

    private static uint _housekeepingKind, _ordinaryHandle, _ordinaryTarget, _housekeepingPlayer;

    [Theory]
    [InlineData(StorageNative.EventRestored, 1u, 901u)]
    [InlineData(StorageNative.EventParked, 0u, 0u)]
    [InlineData(StorageNative.EventParked, 1u, 901u)]
    public unsafe void TemporaryHousekeepingEventsStayRawWithoutReachingThePublicBus(uint kind, uint handle, uint target)
    {
        using var r = new Run();
        r.Register("visible", 8);
        r.Kiln.Step();
        r.Seen.Clear();
        _housekeepingKind = kind;
        _ordinaryHandle = handle;
        _ordinaryTarget = target;
        _housekeepingPlayer = r.Ada.Entity.Value;
        var table = *StorageNative.T;
        table.NextEvent = &TemporaryAndOrdinaryEvents;
        StorageNative.UseForTests(&table);
        try
        {
            StorageNative.Event raw = default;
            raw.Size = (uint)sizeof(StorageNative.Event);
            Assert.Equal(StorageNative.Ok, StorageNative.T->NextEvent(0, &raw));
            Assert.Equal((kind, 0u, 900u), (raw.Kind, raw.Handle, raw.Target));
            r.Kiln.Step();
            var seen = Assert.Single(r.Seen);
            if (kind == StorageNative.EventRestored) Assert.IsType<PlayerStorageRestoredEvent>(seen);
            else Assert.IsType<PlayerStorageParkedEvent>(seen);
            // Filtering the public bus must not consume or rewrite the persistence owner's raw event.
            Assert.Equal(StorageNative.Ok, StorageNative.T->NextEvent(0, &raw));
            Assert.Equal((1UL, 0u, 900u), (raw.Seq, raw.Handle, raw.Target));
        }
        finally { StorageNative.ResetForTests(); }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int TemporaryAndOrdinaryEvents(ulong after, StorageNative.Event* e)
    {
        if (after >= 2) return StorageNative.NotFound;
        *e = default;
        e->Size = (uint)sizeof(StorageNative.Event);
        e->Seq = after + 1;
        e->Kind = _housekeepingKind;
        e->Player = _housekeepingPlayer;
        e->Handle = after == 0 ? 0 : _ordinaryHandle;
        e->Target = after == 0 ? 900 : _ordinaryTarget;
        e->Reason = StorageNative.ParkRequested;
        StorageNative.SetText(e->Key, StorageNative.KeyMax, "quivers:visible");
        return StorageNative.Ok;
    }

    [Fact]
    public void ARegistrationWhileThePlayerIsOnRestoresItsParkedItemsLive()
    {
        using var r = new Run();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada, new HarnessStorageCopy("quivers:spells", 8, [new(2, r.Bolt, 5)]));
        r.Kiln.Step();
        Assert.Equal(PlayerStorageParkReason.Unregistered, Assert.Single(r.Seen.OfType<PlayerStorageParkedEvent>()).Reason);
        var s = r.Register("spells", 8);
        r.Kiln.Step(2);
        Assert.True(r.Kiln.Run(() => s.IsReady(r.Ada.Entity)));
        Assert.Equal(5, r.Kiln.Run(() => s.Read(r.Ada.Entity))!.Count(r.Bolt));
        var live = Assert.Single(r.Seen.OfType<PlayerStorageRestoredEvent>());
        Assert.True(live.Live);
        Assert.Equal(1, live.Stacks);
    }

    [Fact]
    public void WhatDoesNotFitAndWhatKilnCannotUseIsKeptNeverCut()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8, StorageFilter.Categories(ItemCategory.Ammunition));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada, new HarnessStorageCopy("quivers:quiver", 18,
            [new(0, r.Arrow, 10), new(12, r.Arrow, 7), new(1, r.Logs, 5), new(2, new ItemId(0xDEAD), 1), new(3, r.Arrow, 500)]));
        r.Kiln.Step();
        var status = r.Kiln.Run(() => q.Status(r.Ada.Entity));
        Assert.Equal((1, 2, 2), (status.Stacks, status.Parked, status.Quarantined));  // slot 12 > 8 and Logs (filter): parked; unknown, 500 > 99: quarantined
        Assert.Equal(new[] { 12, 1 }, r.Kiln.PlayerStorage.Parked(r.Ada, "quivers:quiver").Select(p => p.Slot).Order().Reverse());
        Assert.Equal(2, r.Kiln.PlayerStorage.Quarantined(r.Ada, "quivers:quiver").Count);
        Assert.Contains(r.Seen.OfType<PlayerStorageParkedEvent>(), e => e.Reason == PlayerStorageParkReason.Overflow);
        Assert.Contains(r.Seen.OfType<PlayerStorageParkedEvent>(), e => e.Reason == PlayerStorageParkReason.Filter);
    }

    [Fact]
    public void TheFilterRefusesAGiveAndHoldersAreFoundOnlyWhenAskedFor()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8, StorageFilter.Ammunition(AmmoKinds.Arrow).NoItemEntities());
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        var holder = r.Kiln.Run(() => q.EntityOf(r.Ada.Entity));
        var logs = r.Kiln.Run(() => Items.Give(holder, r.Logs, 5));
        var bolts = r.Kiln.Run(() => Items.Give(holder, r.Bolt, 5));  // ammunition, but not for the arrow slot
        var arrows = r.Kiln.Run(() => Items.Give(holder, r.Arrow, 30));
        r.Kiln.Step();
        Assert.Equal(ItemsStatus.Filtered, logs.Result.Status);
        Assert.Equal(ItemsStatus.Filtered, bolts.Result.Status);
        Assert.Equal(ItemsStatus.Done, arrows.Result.Status);
        Assert.Equal(30, r.Kiln.Run(() => q.Read(r.Ada.Entity))!.Count(r.Arrow));
        Assert.DoesNotContain(holder, r.Kiln.Run(() => Items.FindInventories()));
        Assert.DoesNotContain(holder, r.Kiln.Run(() => Items.FindInventories(InventoryKinds.All)));
        Assert.Equal(new[] { holder }, r.Kiln.Run(() => Items.FindInventories(InventoryKinds.PlayerStorage)));
        Assert.DoesNotContain(holder, r.Kiln.Run(() => Items.Read(r.Ada.Entity))!.Inventories.Select(i => i.Inventory));
    }

    [Fact]
    public void ParkedItemsGoBackIntoTheGameOnlyWhenAsked()
    {
        using var r = new Run();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada, new HarnessStorageCopy("gone:bag", 8, [new(0, r.Logs, 20), new(5, r.Arrow, 3)]));
        r.Kiln.Step();
        var none = r.Kiln.Run(() => Items.PlayerStorages.ReturnOrphans(r.Ada.Entity, "nothing:here", OrphanTarget.Backpack));
        r.Kiln.Step();
        Assert.Equal(ItemsStatus.NotEnough, none.Result.Status);
        var back = r.Kiln.Run(() => Items.PlayerStorages.ReturnOrphans(r.Ada.Entity, "gone:bag", OrphanTarget.Backpack));
        Assert.False(back.IsCompleted);
        r.Kiln.Step(2);
        var result = back.Result;
        Assert.Equal((ItemsStatus.Done, 23, 2), (result.Status, result.Done, result.Stacks));
        Assert.Equal(20, r.Kiln.Items.Of(r.Ada.Entity)!.Count(r.Logs));
        Assert.Equal(3, r.Kiln.Items.Of(r.Ada.Entity)!.Count(r.Arrow));
        Assert.DoesNotContain(r.Kiln.Run(() => Items.PlayerStorages.Of(r.Ada.Entity)), i => i.FullKey == "gone:bag");
        Assert.Throws<ArgumentException>(() => OrphanTarget.Into(EntityId.None));
        Assert.True(OrphanTarget.Backpack.IsBackpack);
    }

    [Fact]
    public void DeathFollowsEachStoragesRule()
    {
        using var r = new Run();
        var keep = r.Register("keep", 8);
        var grave = r.Register("grave", 8, death: StorageDeath.Grave);
        var follow = r.Register("follow", 8, death: StorageDeath.FollowGame);
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada,
            new HarnessStorageCopy("quivers:keep", 8, [new(0, r.Logs, 5)]),
            new HarnessStorageCopy("quivers:grave", 8, [new(0, r.Arrow, 9)]),
            new HarnessStorageCopy("quivers:follow", 8, [new(0, r.Logs, 4), new(1, r.Arrow, 2)]));
        r.Kiln.Step();
        var tomb = r.Kiln.Entities.Create(new WorldPosition(1, 0, 1));
        r.Kiln.Items.AddContainer(tomb, 2);
        r.Kiln.PlayerStorage.Die(r.Ada, tomb, HarnessTombstoneMode.AddBackpackMaterials);
        r.Kiln.Step();
        var deaths = r.Seen.OfType<PlayerStorageDeathEvent>().ToDictionary(e => e.Key);
        Assert.Equal((0, EntityId.None), (deaths["quivers:keep"].Moved, deaths["quivers:keep"].To));
        Assert.Equal((1, tomb, StorageDeath.Grave), (deaths["quivers:grave"].Moved, deaths["quivers:grave"].To, deaths["quivers:grave"].Rule));
        Assert.Equal((1, 4), (deaths["quivers:follow"].Moved, deaths["quivers:follow"].Items));  // materials only
        Assert.Equal(5, r.Kiln.Run(() => keep.Read(r.Ada.Entity))!.Count(r.Logs));
        Assert.Equal(0, r.Kiln.Run(() => grave.Read(r.Ada.Entity))!.UsedSlots);
        Assert.Equal(2, r.Kiln.Run(() => follow.Read(r.Ada.Entity))!.Count(r.Arrow));
        Assert.Equal(13, r.Kiln.Items.Of(tomb)!.Count(r.Logs) + r.Kiln.Items.Of(tomb)!.Count(r.Arrow));
    }

    [Fact]
    public void StoreNeedsMovesAndANotReadyStorageMovesNothing()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8);
        r.Kiln.Step();
        var early = r.Kiln.Run(() => q.Store(r.Ada.Entity, SlotId.Of(new EntityId(900), 0)));
        Assert.Equal(ItemsStatus.NoInventory, early.Result.Status);
        Assert.Contains("not ready", early.Result.Message);
        var out_ = r.Kiln.Run(() => q.Retrieve(r.Ada.Entity, 0));
        Assert.Equal(ItemsStatus.NoInventory, out_.Result.Status);
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        // the kit has no moves (items.move): a ready storage hands the move to Items.Move, which says so
        void StoreNow() => r.Kiln.Run(() => { _ = q.Store(r.Ada.Entity, SlotId.Of(new EntityId(900), 0)); });
        Assert.Equal("items.move", Assert.Throws<KilnUnavailableException>(StoreNow).Capability);
    }

    [Fact]
    public void ALeaveTakesTheStoragesAway()
    {
        using var r = new Run();
        var q = r.Register("quiver", 8);
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        var holder = r.Kiln.Run(() => q.EntityOf(r.Ada.Entity));
        r.Kiln.PlayerStorage.Leave(r.Ada);
        r.Kiln.Step();
        Assert.False(r.Kiln.Run(() => q.IsReady(r.Ada.Entity)));
        Assert.Null(r.Kiln.Run(() => Items.Read(holder)));
    }

    // ---- the filter ----

    [Fact]
    public void FiltersCombineAndCompileToWhatKilnItemsChecks()
    {
        Assert.Equal("any item", StorageFilter.Any.ToString());
        Assert.Equal(default, StorageFilter.Any);
        var ammo = StorageFilter.Categories(ItemCategory.Ammunition, ItemCategory.Materials).And(StorageFilter.Categories(ItemCategory.Ammunition));
        Assert.Equal(1u << (int)ItemCategory.Ammunition, ammo.CategoryBits);
        Assert.Throws<ArgumentException>(() => StorageFilter.Categories(ItemCategory.Weapons).And(StorageFilter.Categories(ItemCategory.Materials)));
        Assert.Throws<ArgumentException>(() => StorageFilter.Ammunition(AmmoKinds.Arrow).And(StorageFilter.Ammunition(AmmoKinds.Spell)));
        Assert.Throws<ArgumentException>(() => StorageFilter.Categories());
        Assert.Throws<ArgumentException>(() => StorageFilter.Ammunition(0));
        Assert.Throws<ArgumentException>(() => StorageFilter.Items(ItemId.None));
        var items = StorageFilter.Items(new ItemId(9), new ItemId(3), new ItemId(9));
        Assert.Equal(new[] { 3u, 9u }, items.Compile([]).Ids);
        Assert.Throws<ArgumentException>(() => items.And(StorageFilter.Items(new ItemId(4))));
        var both = StorageFilter.Ammunition(AmmoKinds.Arrow | AmmoKinds.Spell).NoItemEntities();
        Assert.Equal((0u, 1u | 2u | 4u), (both.Compile([]).Categories, both.Compile([]).Flags));
        Assert.Throws<ArgumentException>(() => StorageFilter.Items([.. Enumerable.Range(1, 1025).Select(i => new ItemId((uint)i))]));
    }

    [Fact]
    public void WhereIsAskedOnceOverTheCatalog()
    {
        var calls = 0;
        var catalog = new List<ItemInfo>
        {
            new(new ItemId(5), "Ammo_A", null, 99, ItemCategory.Ammunition, ItemRarity.Common, false, 0, 0, 0, 0, Guid.Empty),
            new(new ItemId(2), "Ammo_B", null, 99, ItemCategory.Ammunition, ItemRarity.Common, false, 0, 0, 0, 0, Guid.Empty),
            new(new ItemId(7), "Sword", null, 1, ItemCategory.Weapons, ItemRarity.Common, true, 1, 5, 0, 0, Guid.Empty),
        };
        var f = StorageFilter.Where(i => { calls++; return i.Name.StartsWith("Ammo", StringComparison.Ordinal) || i.IsGear; });
        Assert.Equal(new[] { 2u, 5u, 7u }, f.Compile(catalog).Ids);
        Assert.Equal(3, calls);
        Assert.Equal(new[] { 2u, 5u }, f.NoItemEntities().Compile(catalog).Ids);  // gear is left out of the list
        Assert.Equal(new[] { 5u }, f.And(StorageFilter.Items(new ItemId(5), new ItemId(7))).And(StorageFilter.Categories(ItemCategory.Ammunition)).Compile(catalog).Ids);
        Assert.Throws<ArgumentException>(() => StorageFilter.Where(_ => false).Compile(catalog));
    }

    [Fact]
    public void AWhereFilterWaitsForTheCatalog()
    {
        using var r = new Run(catalog: false);
        var q = r.Register("quiver", 8, StorageFilter.Where(i => i.Category == ItemCategory.Ammunition));
        r.Kiln.Step();
        r.Kiln.PlayerStorage.JoinAndRestore(r.Ada);
        r.Kiln.Step();
        Assert.Equal(PlayerStorageState.Waiting, r.Kiln.Run(() => q.Status(r.Ada.Entity)).State);  // its filter waits for the catalog
        Assert.DoesNotContain(r.Kiln.Run(() => Items.PlayerStorages.Of(r.Ada.Entity)), i => i.FullKey == "quivers:quiver");
        var arrow = r.Kiln.Items.AddItem("Ammo_Arrow_T1", 99, ItemCategory.Ammunition);
        r.Kiln.Step(3);
        Assert.True(r.Kiln.Run(() => q.IsReady(r.Ada.Entity)));
        var holder = r.Kiln.Run(() => q.EntityOf(r.Ada.Entity));
        var give = r.Kiln.Run(() => Items.Give(holder, arrow, 4));
        r.Kiln.Step();
        Assert.Equal(ItemsStatus.Done, give.Result.Status);
    }

    // ---- the client copy: off without kiln.net (the kit has none; PlayerStorageMirrorTests puts a loopback in its place) ----

    [Fact]
    public void TheClientCopyIsOffAndSaysSo()
    {
        using var r = new Run();
        Assert.False(r.Kiln.Run(() => PlayerStorage.Client.Available));
        Assert.Equal("items.storage.client", Assert.Throws<KilnUnavailableException>(() => r.Kiln.Run(() => PlayerStorage.Client.Rows("quivers:quiver"))).Capability);
        Assert.Equal(CapabilityState.Unavailable, r.Kiln.Run(() => Capabilities.Get("items.storage.client")).State);
    }
}
