// SPDX-License-Identifier: MIT
// Items.GearOf / SetRarity / SetEnhancements / SetLevel and the gear events offline: the ABI mirrors, the request a
// call sends, the state and result built from native ones (the one error rule), the capability verdict, the argument and
// capability errors, and the whole path on the test kit's kiln.items (its gear model ports native/services/items/
// gear_logic.h): refusals per status, lowering below the unlocked count, stale refs, events once per change, the game's
// own change through the gear watch, and no event loop from a handler. What the game does is the lab self-test of
// package P1 (native/services/items/gear.cpp) and the client checks of package P4.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed unsafe class GearTests
{
    private const ulong AdaSteam = 76561198000000901;

    // The calls return a task but throw before one exists: run them as a plain call.
    private static void Call(Func<Task> f) => _ = f();

    private static ItemInstanceRef Stamped(uint pide, uint item, uint stamp) =>
        new(new EntityRef(new EntityId(pide), IdentitySources.Stamp(EntitySourceOrder.Items, stamp)), new ItemId(item));

    // ---- the ABI ----

    [Fact]
    public void TheMirrorsHaveTheHeadersSizes()
    {
        Assert.Equal(40, sizeof(ItemsNative.GearRequest));
        Assert.Equal(44, sizeof(ItemsNative.GearState));
        Assert.Equal(296, sizeof(ItemsNative.GearResult));
        Assert.Equal(192, sizeof(ItemsNative.GearStatus));
        Assert.Equal(368, sizeof(ItemsNative.TableV8));  // native items.cpp: sizeof(KilnItemsApi) == 368 (gear, P5's rarity_rolls/roll_status, P3's gear_upgrade, then P7's scales)
        Assert.Equal((int)ItemsNative.V8GearEnd, (int)System.Runtime.InteropServices.Marshal.OffsetOf<ItemsNative.TableV8>(nameof(ItemsNative.TableV8.RarityRolls)));  // the gear functions end at 320
        Assert.Equal(296, (int)System.Runtime.InteropServices.Marshal.OffsetOf<ItemsNative.TableV8>(nameof(ItemsNative.TableV8.GearWrite)));  // offsetof(KilnItemsApi, gear_write) == 296
    }

    // ---- the request ----

    [Fact]
    public void ARarityIsItsSlotsWithTheDurabilityRule()
    {
        var item = Stamped(5000, 0x1234, 7);
        var r = Items.GearRequestOf(item, 7, Items.GearChange.ToRarity(ItemRarity.Epic), GearWriteOptions.Default);
        Assert.Equal((40u, 5000u, 7u, 0x1234u, (byte)4, (byte)0, 0u), (r.Size, r.Entity, r.Stamp, r.Item, r.Available, r.Unlocked, r.Level));
        Assert.Equal(ItemsNative.GearAvailable | ItemsNative.GearDurabilityRefresh, r.Flags);
        Assert.Equal(ItemsNative.GearAvailable, Items.GearRequestOf(item, 7, Items.GearChange.ToRarity(ItemRarity.Common),
            new GearWriteOptions { Durability = DurabilityRule.Keep }).Flags);
        var all = Items.GearRequestOf(item, 7, Items.GearChange.ToRarity(ItemRarity.Legendary),
            new GearWriteOptions { Durability = DurabilityRule.Repair, AllowEmptySlot = true, RefuseEquipped = true, DryRun = true });
        Assert.Equal(5, all.Available);
        Assert.Equal(ItemsNative.GearAvailable | ItemsNative.GearDurabilityRepair | ItemsNative.GearForceSlot | ItemsNative.GearNotEquipped |
                     ItemsNative.GearDryRun, all.Flags);
    }

    [Fact]
    public void UnlockedAndLevelSendOnlyTheirOwnField()
    {
        var item = Stamped(5000, 0, 7);
        var u = Items.GearRequestOf(item, 7, Items.GearChange.ToUnlocked(0), new GearWriteOptions { Durability = DurabilityRule.Keep });
        Assert.Equal((ItemsNative.GearUnlocked, (byte)0, (byte)0, 0u), (u.Flags, u.Available, u.Unlocked, u.Level));
        var l = Items.GearRequestOf(item, 7, Items.GearChange.ToLevel(255), new GearWriteOptions { Durability = DurabilityRule.Keep });
        Assert.Equal((ItemsNative.GearLevel, 255u), (l.Flags, l.Level));
        var both = Items.GearRequestOf(item, 7, new Items.GearChange(4, 4, null), new GearWriteOptions { Durability = DurabilityRule.Keep });
        Assert.Equal((ItemsNative.GearAvailable | ItemsNative.GearUnlocked, (byte)4, (byte)4), (both.Flags, both.Available, both.Unlocked));
        Assert.Throws<ArgumentOutOfRangeException>(() => Items.GearRequestOf(item, 7, new Items.GearChange(0, null, null), GearWriteOptions.Default));
        Assert.Throws<ArgumentOutOfRangeException>(() => Items.GearRequestOf(item, 7, new Items.GearChange(null, 6, null), GearWriteOptions.Default));
        Assert.Throws<ArgumentOutOfRangeException>(() => Items.GearRequestOf(item, 7, new Items.GearChange(null, null, 256), GearWriteOptions.Default));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            Items.GearRequestOf(item, 7, Items.GearChange.ToLevel(3), new GearWriteOptions { Durability = (DurabilityRule)9 }));
    }

    [Fact]
    public void ARefMustBeAStampedItemEntity()
    {
        Assert.Equal(7u, Items.StampOf(Stamped(5000, 1, 7), "item"));
        Assert.Throws<ArgumentException>(() => Items.StampOf(ItemInstanceRef.None, "item"));
        Assert.Throws<ArgumentException>(() => Items.StampOf(new ItemInstanceRef(new EntityRef(new EntityId(5000), 0), new ItemId(1)), "item"));
        var spawnStamp = new ItemInstanceRef(new EntityRef(new EntityId(5000), IdentitySources.Stamp(EntitySourceOrder.Spawn, 7)), new ItemId(1));
        Assert.Throws<ArgumentException>(() => Items.StampOf(spawnStamp, "item"));
    }

    // ---- states and results ----

    private static ItemsNative.GearState Native(byte available, byte unlocked, uint level, byte maxAvailable = 5, byte typeRarity = 2,
        uint flags = ItemsNative.GearHasPerks | ItemsNative.GearHasLevel | ItemsNative.GearHasDurability, ushort state = 0x12) => new()
    {
        Flags = flags, Item = 0x1234, Available = available, Unlocked = unlocked,
        Rarity = (flags & ItemsNative.GearHasPerks) != 0 && available is >= 1 and <= 5 ? (byte)(available - 1) : typeRarity,
        TypeRarity = typeRarity, MaxAvailable = maxAvailable, RarityUi = 2, LevelUi = 6, Level = level, LevelMax = level,
        TypeLevelMin = 5, TypeLevelMax = 5, Durability = 50, DurabilityMax = 100, State = state,
    };

    [Fact]
    public void AStateSaysRarityLevelsAndSlots()
    {
        var item = Stamped(5000, 0x1234, 7);
        var g = Items.StateOf(item, Native(3, 1, 6));
        Assert.Equal((ItemRarity.Rare, ItemRarity.Rare, ItemRarity.Legendary), (g.Rarity, g.TypeRarity, g.MaxRarity));
        Assert.Equal((6, 5, 3, 1, 8), (g.Level, g.BaseLevel, g.Enhancements, g.Unlocked, g.MaxLevel));
        Assert.Equal((50, 100), (g.Durability, g.DurabilityMax));
        Assert.True(g.HasEnhancementSlots);
        Assert.False(g.HasSocket);
        Assert.Equal((ItemRarity.Rare, 6, 5, 5), (g.ShownRarity, g.ShownLevel, g.TypeLevelMin, g.TypeLevelMax));
        Assert.Equal([true, true, true, false, false], g.Perks.Select(p => p.Available));
        Assert.Equal([true, false, false, false, false], g.Perks.Select(p => p.Unlocked));
        Assert.All(g.Perks, p => Assert.True(p.TypeHasSlot));
        Assert.Equal(item, g.Item);
    }

    [Fact]
    public void AnItemWithoutSlotsHasItsTypesRarityAndNoDurabilityWhenItHasNone()
    {
        var g = Items.StateOf(Stamped(5000, 0, 7), Native(0, 0, 0, maxAvailable: 0, typeRarity: 1, flags: 0, state: 0x10));
        Assert.Equal((ItemRarity.Uncommon, ItemRarity.Uncommon, 0, 0), (g.Rarity, g.MaxRarity, g.Enhancements, g.Unlocked));
        Assert.False(g.HasEnhancementSlots);
        Assert.Null(g.Durability);
        Assert.Null(g.DurabilityMax);
        Assert.Equal(6, g.Level);  // no Level attribute: the shown level
        Assert.Equal(new ItemId(0x1234), g.Item.Item);  // a ref without a type gets the one the read found
        Assert.All(g.Perks, p => Assert.False(p.TypeHasSlot));
        var socket = Items.StateOf(Stamped(5000, 0, 7), Native(1, 0, 5, maxAvailable: 3, state: Items.ItemStateHasGemSlot));
        Assert.True(socket.HasSocket);
        Assert.Equal(ItemRarity.Rare, socket.MaxRarity);
        Assert.Equal([true, true, true, false, false], socket.Perks.Select(p => p.TypeHasSlot));
    }

    private static ItemsNative.GearResult Result(int status, uint changed = 0, uint flags = 0, bool ran = true, string message = "")
    {
        var n = new ItemsNative.GearResult { Size = 296, Status = status, Ticket = 9, Entity = 5000, Stamp = 7, Flags = flags, Changed = changed, Steps = 1, Step = 40 };
        if (ran)
        {
            n.Before = Native(3, 0, 5);
            n.After = Native(4, 0, 5);
        }
        var bytes = System.Text.Encoding.UTF8.GetBytes(message);
        for (var i = 0; i < bytes.Length && i < 159; i++) n.Message[i] = bytes[i];
        return n;
    }

    [Fact]
    public void EveryNativeStatusIsAGearStatusOfTheSameValue()
    {
        foreach (var (native, status) in new[]
                 {
                     (ItemsNative.GearDone, GearStatus.Done), (ItemsNative.GearStale, GearStatus.Stale), (ItemsNative.GearNotGear, GearStatus.NotGear),
                     (ItemsNative.GearNoSlot, GearStatus.NoSlot), (ItemsNative.GearOutOfRange, GearStatus.OutOfRange), (ItemsNative.GearEquipped, GearStatus.Equipped),
                     (ItemsNative.GearUnavailableStatus, GearStatus.Unavailable), (ItemsNative.GearDropped, GearStatus.Dropped), (ItemsNative.GearRefused, GearStatus.Refused),
                 })
        {
            Assert.Equal(native, (int)status);
            Assert.Equal(status, Items.GearResultOf(Stamped(5000, 0x1234, 7), Result(native)).Status);
        }
        Assert.Equal(GearStatus.Refused, Items.GearResultOf(Stamped(5000, 0x1234, 7), Result(99)).Status);  // an unknown status is a refusal
    }

    [Fact]
    public void AResultFollowsTheErrorRule()
    {
        var item = Stamped(5000, 0x1234, 7);
        var done = Items.GearResultOf(item, Result(0, ItemsNative.GearChangedPerks | ItemsNative.GearChangedState));
        IKilnResult k = done;
        Assert.True(k.Ok);
        Assert.Null(k.Reason);
        Assert.Equal(GearChanges.Enhancements | GearChanges.DurabilityRefresh, done.Changed);
        Assert.Equal((ItemRarity.Rare, ItemRarity.Epic), (done.Before!.Rarity, done.After!.Rarity));
        Assert.Equal(1, done.Steps);
        Assert.False(done.DryRun);
        Assert.True(Items.GearResultOf(item, Result(0, flags: ItemsNative.GearDryRun)).DryRun);
        var noSlot = Items.GearResultOf(item, Result(ItemsNative.GearNoSlot, ItemsNative.GearChangedPerks, message: "the item's type has no perk in that enhancement slot"));
        Assert.False(noSlot.Ok);
        Assert.Equal("the item's type has no perk in that enhancement slot", noSlot.Reason);
        Assert.Equal(GearChanges.None, noSlot.Changed);  // a refusal changed nothing
        var stale = Items.GearResultOf(item, Result(ItemsNative.GearStale, ran: false));
        Assert.Null(stale.Before);
        Assert.Null(stale.After);
        Assert.Equal("Stale", stale.Reason);
    }

    // ---- the capability ----

    [Fact]
    public void TheCapabilityVerdictFollowsTheTableTheFeatureAndTheProgram()
    {
        Assert.Equal(Items.GearVerdict.NoTable, Items.DecideGear(null, false, false, false, "").Verdict);
        var older = Items.DecideGear(7, false, true, false, "");
        Assert.Equal(Items.GearVerdict.NoV8, older.Verdict);
        Assert.Contains("v7 has no gear", older.Text);
        Assert.Equal(Items.GearVerdict.NoFeature, Items.DecideGear(8, true, false, true, "").Verdict);
        var off = Items.DecideGear(8, true, true, false, "gear writes need kiln.world");
        Assert.Equal((Items.GearVerdict.WritesOff, "gear writes need kiln.world"), off);
        Assert.Equal(Items.GearVerdict.WritesOff, Items.DecideGear(8, true, true, false, "").Verdict);
        Assert.Equal(Items.GearVerdict.On, Items.DecideGear(8, true, true, true, "running").Verdict);
    }

    [Fact]
    public void ArgumentsAreCheckedBeforeAnythingElse()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var item = Stamped(5000, 1, 7);
        kiln.Run(() =>
        {
            Assert.Throws<ArgumentException>(() => Call(() => Items.SetRarity(ItemInstanceRef.None, ItemRarity.Rare)));
            Assert.Throws<ArgumentOutOfRangeException>(() => Call(() => Items.SetRarity(item, (ItemRarity)5)));
            Assert.Throws<ArgumentOutOfRangeException>(() => Call(() => Items.SetEnhancements(item, 6)));
            Assert.Throws<ArgumentOutOfRangeException>(() => Call(() => Items.SetEnhancements(item, -1)));
            Assert.Throws<ArgumentOutOfRangeException>(() => Call(() => Items.SetLevel(item, 0)));
            Assert.Throws<ArgumentOutOfRangeException>(() => Call(() => Items.SetLevel(item, 256)));
            Assert.Throws<ArgumentException>(() => Items.GearOf(new ItemInstanceRef(new EntityRef(new EntityId(5000), 0), new ItemId(1))));
        });
    }

    [Fact]
    public void OffTheTickThreadThrows()
    {
        using var kiln = new KilnHarness();
        Assert.Throws<InvalidOperationException>(() => Items.GearOf(Stamped(5000, 1, 7)));
        Assert.Throws<InvalidOperationException>(() => Call(() => Items.SetRarity(Stamped(5000, 1, 7), ItemRarity.Rare)));
    }

    // ---- on the test kit's kiln.items ----

    private sealed class Run : IDisposable
    {
        public readonly KilnHarness Kiln = new();
        public readonly FakePlayer Ada;
        public readonly ItemId Sword;
        public readonly List<GearEvent> Events = [];
        private readonly List<IDisposable> _subs = [];

        public Run(int perkSlots = 5)
        {
            Sword = Kiln.Items.AddItem("Weapon_T1_1H_Sword_Test", 1, ItemCategory.Weapons, gear: true, rarity: ItemRarity.Common, levelMin: 5, levelMax: 5);
            if (perkSlots != 5) Kiln.Items.SetPerkSlots(Sword, perkSlots);
            Ada = Kiln.Players.Join("Ada", AdaSteam, position: new WorldPosition(0, 0, 0));
            Kiln.Step(2);
        }

        public void Listen()
        {
            _subs.Add(Kiln.Run(() => global::Kiln.Events.On<GearChangedEvent>(Events.Add)));
            _subs.Add(Kiln.Run(() => global::Kiln.Events.On<GearRarityChangedEvent>(Events.Add)));
            _subs.Add(Kiln.Run(() => global::Kiln.Events.On<GearEnhancedEvent>(Events.Add)));
        }

        public ItemInstanceRef GiveSword(ItemRarity rarity)
        {
            var give = Kiln.Run(() => Items.Give(Ada.Entity, Sword, 1, new Gear(5, rarity)));
            Kiln.Step();
            var r = give.Result;
            Assert.True(r.Succeeded, r.ToString());
            var item = Assert.Single(r.Changes).Ref;
            Assert.False(item.IsNone);
            return item;
        }

        public GearResult Write(Func<Task<GearResult>> call)
        {
            var t = Kiln.Run(call);
            Assert.False(t.IsCompleted);  // the write runs at the start of the next step
            Kiln.Step();
            Assert.True(t.IsCompletedSuccessfully);
            return t.Result;
        }

        public GearState State(ItemInstanceRef item) => Kiln.Run(() => Items.GearOf(item)) ?? throw new Xunit.Sdk.XunitException("no gear");

        public void Dispose()
        {
            foreach (var s in _subs) s.Dispose();
            Kiln.Dispose();
        }
    }

    [Fact]
    public void TheRowsAreOnInTheKit()
    {
        using var run = new Run();
        Assert.True(run.Kiln.Run(() => Items.GearReadAvailable && Items.GearWriteAvailable && Capabilities.Is("items.gear.durability")),
            run.Kiln.Run(() => Capabilities.Why("items.gear.write")));
    }

    [Fact]
    public void AGiftsGearReadsAsTheGameMadeIt()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        var g = run.State(item);
        Assert.Equal((ItemRarity.Rare, ItemRarity.Common, ItemRarity.Legendary, 3, 0, 5, 5, 8), (g.Rarity, g.TypeRarity, g.MaxRarity, g.Enhancements, g.Unlocked, g.Level, g.BaseLevel, g.MaxLevel));
        Assert.Equal((100, 100), (g.Durability, g.DurabilityMax));
        Assert.Equal(ItemRarity.Rare, run.Kiln.Run(() => Items.RarityOf(item)));
        var slot = run.Ada.Inventory.Slots.Single(s => s.Item == run.Sword);
        Assert.Equal(item, slot.Ref);
        Assert.Equal(item, run.Kiln.Run(() => Items.Read(run.Ada.Entity)!.Slots.Single(s => s.Item == run.Sword).Ref));
    }

    [Fact]
    public void SetRarityRaisesInPlaceAndTheGameShowsItAStepLater()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Listen();
        var pide = item.Entity.Id;
        var r = run.Write(() => Items.SetRarity(item, ItemRarity.Epic));
        Assert.True(r.Ok, r.ToString());
        Assert.Equal(GearChanges.Enhancements | GearChanges.DurabilityRefresh, r.Changed);
        Assert.Equal((ItemRarity.Rare, ItemRarity.Epic, 4, 0), (r.Before!.Rarity, r.After!.Rarity, r.After.Enhancements, r.After.Unlocked));
        Assert.Equal(ItemRarity.Rare, r.After.ShownRarity);  // the game derives the shown rarity in its next step
        var now = run.State(item);
        Assert.Equal((ItemRarity.Epic, 4), (now.Rarity, now.Enhancements));
        run.Kiln.Step();
        Assert.Equal(ItemRarity.Epic, run.State(item).ShownRarity);
        Assert.Equal(ItemRarity.Epic, run.Ada.Inventory.Slots.Single(s => s.Item == run.Sword).Rarity);
        Assert.Equal(pide, run.Ada.Inventory.Slots.Single(s => s.Item == run.Sword).ItemEntity);  // the same item entity
        // one change, one event of each kind that applies: changed, rarity; not enhanced (nothing was unlocked)
        Assert.Equal(["GearChangedEvent Code kiln", "GearRarityChangedEvent Code kiln"], run.Events.Select(e => $"{e.GetType().Name} {e.Source} {e.Origin}"));
        var rarity = run.Events.OfType<GearRarityChangedEvent>().Single();
        Assert.Equal((ItemRarity.Rare, ItemRarity.Epic, item), (rarity.From, rarity.To, rarity.Item));
        Assert.Equal(GearChanges.Enhancements, run.Events.OfType<GearChangedEvent>().Single().Changed);
        run.Kiln.Step(10);  // the gear watch reads Ada's items: Kiln's own change is not reported again as the game's
        Assert.Equal(2, run.Events.Count);
    }

    [Fact]
    public void LoweringBelowTheUnlockedCountTakesUnlockedAndLevelsAlong()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        var u = run.Write(() => Items.SetEnhancements(item, 3));
        Assert.True(u.Ok, u.ToString());
        Assert.Equal((3, 8, 8), (u.After!.Unlocked, u.After.Level, u.After.MaxLevel));
        run.Listen();
        var down = run.Write(() => Items.SetRarity(item, ItemRarity.Uncommon));
        Assert.True(down.Ok, down.ToString());
        Assert.Equal((ItemRarity.Uncommon, 2, 2, 7, 5), (down.After!.Rarity, down.After.Enhancements, down.After.Unlocked, down.After.Level, down.After.BaseLevel));
        Assert.Equal(["GearChangedEvent", "GearRarityChangedEvent", "GearEnhancedEvent"], run.Events.Select(e => e.GetType().Name));
        var enhanced = run.Events.OfType<GearEnhancedEvent>().Single();
        Assert.Equal((3, 2), (enhanced.From, enhanced.To));
        Assert.Equal(GearChanges.Enhancements | GearChanges.Level, run.Events.OfType<GearChangedEvent>().Single().Changed);
    }

    [Fact]
    public void EveryRefusalIsAResultAndChangesNothing()
    {
        using var run = new Run(perkSlots: 3);
        var item = run.GiveSword(ItemRarity.Rare);
        run.Listen();
        var noSlot = run.Write(() => Items.SetRarity(item, ItemRarity.Epic));
        Assert.Equal(GearStatus.NoSlot, noSlot.Status);
        Assert.False(noSlot.Ok);
        Assert.Contains("no perk", noSlot.Reason);
        Assert.Equal(ItemRarity.Rare, noSlot.After!.Rarity);
        Assert.Equal(ItemRarity.Rare, run.State(item).MaxRarity);
        Assert.Equal(GearStatus.OutOfRange, run.Write(() => Items.SetEnhancements(item, 4)).Status);
        Assert.Equal(GearStatus.OutOfRange, run.Write(() => Items.SetLevel(item, 9)).Status);  // base 9 is outside the type's 5..5
        Assert.Empty(run.Events);
        Assert.Equal((ItemRarity.Rare, 0, 5), (run.State(item).Rarity, run.State(item).Unlocked, run.State(item).Level));
        // an empty slot only when the mod says so
        var forced = run.Write(() => Items.SetRarity(item, ItemRarity.Legendary, new GearWriteOptions { AllowEmptySlot = true }));
        Assert.True(forced.Ok, forced.ToString());
        Assert.Equal(5, run.State(item).Enhancements);
        Assert.False(run.State(item).Perks[4].TypeHasSlot);
    }

    [Fact]
    public void AStaleRefReadsNullAndWritesStale()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        var take = run.Kiln.Run(() => Items.Take(run.Ada.Entity, run.Sword, 1));
        run.Kiln.Step();
        Assert.True(take.Result.Succeeded);
        Assert.Null(run.Kiln.Run(() => Items.GearOf(item)));
        Assert.Null(run.Kiln.Run(() => Items.RarityOf(item)));
        var r = run.Write(() => Items.SetRarity(item, ItemRarity.Epic));
        Assert.Equal(GearStatus.Stale, r.Status);
        Assert.Null(r.Before);
        // a ref with the right id and another stamp is stale too
        var again = run.GiveSword(ItemRarity.Rare);
        var forged = Stamped(again.Entity.Id.Value, again.Item.Value, (uint)(again.Entity.Serial & IdentitySources.MaxStampValue) + 1000);
        Assert.Null(run.Kiln.Run(() => Items.GearOf(forged)));
        Assert.Equal(GearStatus.Stale, run.Write(() => Items.SetLevel(forged, 5)).Status);
    }

    [Fact]
    public void ADryRunOrNoChangePostsNothing()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Listen();
        var dry = run.Write(() => Items.SetRarity(item, ItemRarity.Legendary, new GearWriteOptions { DryRun = true }));
        Assert.True(dry.Ok);
        Assert.True(dry.DryRun);
        Assert.Equal(ItemRarity.Legendary, dry.After!.Rarity);
        Assert.Equal(ItemRarity.Rare, run.State(item).Rarity);
        var same = run.Write(() => Items.SetRarity(item, ItemRarity.Rare));
        Assert.True(same.Ok);
        Assert.Equal(GearChanges.DurabilityRefresh, same.Changed);  // the game is told to compute max durability again; nothing shown changes
        var keep = run.Write(() => Items.SetRarity(item, ItemRarity.Rare, new GearWriteOptions { Durability = DurabilityRule.Keep }));
        Assert.Equal(GearChanges.None, keep.Changed);
        Assert.Empty(run.Events);
    }

    [Fact]
    public void RepairFillsDurabilityAndPostsAChange()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Kiln.Items.SetGear(item, durability: 40);
        run.Listen();
        var r = run.Write(() => Items.SetLevel(item, 5, new GearWriteOptions { Durability = DurabilityRule.Repair }));
        Assert.True(r.Ok, r.ToString());
        Assert.Equal(GearChanges.Durability | GearChanges.DurabilityRefresh, r.Changed);
        Assert.Equal((40, 100), (r.Before!.Durability, r.After!.Durability));
        Assert.Equal(GearChanges.Durability, Assert.IsType<GearChangedEvent>(Assert.Single(run.Events)).Changed);
    }

    [Fact]
    public void TheGamesOwnUnlockIsSeenOnceAsTheGames()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Listen();
        run.Kiln.Step(3);  // the gear watch reads Ada's items: the baseline, no event
        Assert.Empty(run.Events);
        run.Kiln.Items.SetGear(item, unlocked: 1, level: 6);  // a workbench unlock
        run.Kiln.Step(3);
        Assert.Equal(["GearChangedEvent Game game", "GearEnhancedEvent Game game"], run.Events.Select(e => $"{e.GetType().Name} {e.Source} {e.Origin}"));
        var e = run.Events.OfType<GearEnhancedEvent>().Single();
        Assert.Equal((0, 1, 5, 6, item), (e.From, e.To, e.Before.Level, e.After.Level, e.Item));
        run.Kiln.Step(5);
        Assert.Equal(2, run.Events.Count);  // once
        run.Kiln.Items.SetGear(item, durability: 10);  // wear is no gear change
        run.Kiln.Step(3);
        Assert.Equal(2, run.Events.Count);
    }

    [Fact]
    public void AHandlerThatWritesAgainGetsItsEventInALaterStepNeverInside()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        var seen = new List<(ulong Step, ItemRarity To)>();
        var depth = 0;
        var maxDepth = 0;
        using var sub = run.Kiln.Run(() => global::Kiln.Events.On<GearRarityChangedEvent>(e =>
        {
            depth++;
            maxDepth = Math.Max(maxDepth, depth);
            seen.Add((run.Kiln.CurrentStep, e.To));
            if (e.To == ItemRarity.Epic) _ = Items.SetRarity(e.Item, ItemRarity.Legendary);  // a handler answering with a write
            depth--;
        }));
        _ = run.Kiln.Run(() => Items.SetRarity(item, ItemRarity.Epic));
        run.Kiln.Step(5);
        Assert.Equal([ItemRarity.Epic, ItemRarity.Legendary], seen.Select(s => s.To));
        Assert.True(seen[1].Step > seen[0].Step);
        Assert.Equal(1, maxDepth);
        Assert.Equal(ItemRarity.Legendary, run.State(item).Rarity);
    }

    [Fact]
    public void ASwitchedOffFeatureThrowsNamingTheRow()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Kiln.Capabilities.Override("items.gear", false, "test: off");
        var read = run.Kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Items.GearOf(item)));
        Assert.Equal("items.gear.read", read.Capability);
        var write = run.Kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Call(() => Items.SetRarity(item, ItemRarity.Epic))));
        Assert.Equal("items.gear.write", write.Capability);
        run.Kiln.Step(26);  // the rows follow within 25 steps
        Assert.False(run.Kiln.Run(() => Items.GearReadAvailable));
        Assert.Contains("needs items.gear", run.Kiln.Run(() => Capabilities.Why("items.gear.durability")));
    }

    [Fact]
    public void AWriteSwitchedOffAfterTheCallEndsUnavailable()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        var t = run.Kiln.Run(() => Items.SetRarity(item, ItemRarity.Epic));
        run.Kiln.Capabilities.Override("items.gear", false);
        run.Kiln.Step();
        Assert.Equal(GearStatus.Unavailable, t.Result.Status);
        run.Kiln.Capabilities.ClearOverride("items.gear");
        Assert.Equal(ItemRarity.Rare, run.State(item).Rarity);
    }
}
