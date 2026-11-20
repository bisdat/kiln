// SPDX-License-Identifier: MIT
// Items.UpgradeRarity offline: the ABI mirrors, the rule checks, the level-scaled cost, the native request a success or a
// failed roll sends, the status mapping, and the whole path on the test kit's kiln.items (its upgrade model ports
// native/services/items/upgrade_logic.h and gear.cpp's upgrade): the cost taken with the write or not at all, a chest that
// pays, too little to pay, each failure rule with a named roll, refusals (no slot, at the top, a rule that does not take
// the item), the events (cancel, a handler's price and chance), Unlock, a level-scaled price, a dry run, a rule by name,
// and two upgrades of one item in one step. What the game does is the lab test mods/tests/upgrades-test.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed unsafe class UpgradeTests
{
    private const ulong AdaSteam = 76561198000000902;

    private static void Call(Func<Task> f) => _ = f();

    // ---- the ABI ----

    [Fact]
    public void TheMirrorsHaveTheHeadersSizes()
    {
        Assert.Equal(160, sizeof(ItemsNative.UpgradeRequest));
        Assert.Equal(392, sizeof(ItemsNative.UpgradeResult));
        Assert.Equal(24, (int)System.Runtime.InteropServices.Marshal.OffsetOf<ItemsNative.UpgradeRequest>(nameof(ItemsNative.UpgradeRequest.Gear)));
        Assert.Equal(96, (int)System.Runtime.InteropServices.Marshal.OffsetOf<ItemsNative.UpgradeResult>(nameof(ItemsNative.UpgradeResult.Gear)));
        Assert.Equal(368, sizeof(ItemsNative.TableV8));  // native items.cpp: sizeof(KilnItemsApi) == 368 (P7's scales after gear_upgrade at 336)
        Assert.Equal((int)ItemsNative.V8RollsEnd, (int)System.Runtime.InteropServices.Marshal.OffsetOf<ItemsNative.TableV8>(nameof(ItemsNative.TableV8.GearUpgrade)));
    }

    // ---- pure parts ----

    private static RarityUpgradeRule Rule(params (ItemRarity To, UpgradeStep Step)[] steps)
    {
        var rule = new RarityUpgradeRule();
        foreach (var (to, step) in steps) rule.Steps[to] = step;
        return rule;
    }

    [Fact]
    public void AMalformedRuleThrowsNamingTheField()
    {
        void Bad(RarityUpgradeRule r, string field) => Assert.Contains(field, Assert.Throws<ArgumentException>(() => ItemUpgrades.Check(r, "rule")).Message);
        Bad(Rule((ItemRarity.Rare, new UpgradeStep { Chance = 1.5 })), "Steps[Rare].Chance");
        Bad(Rule((ItemRarity.Rare, new UpgradeStep { Chance = double.NaN })), "Steps[Rare].Chance");
        Bad(Rule((ItemRarity.Common, new UpgradeStep())), "Steps[Common]");
        Bad(Rule((ItemRarity.Rare, new UpgradeStep { LevelScale = -1 })), "LevelScale");
        Bad(Rule((ItemRarity.Rare, new UpgradeStep { Cost = { ["Shard"] = 0 } })), "Cost[Shard]");
        Bad(Rule((ItemRarity.Rare, new UpgradeStep { Cost = { ["Shard"] = 100001 } })), "Cost[Shard]");
        var many = new UpgradeStep();
        for (var i = 0; i < 9; i++) many.Cost[$"Item{i}"] = 1;
        Bad(Rule((ItemRarity.Rare, many)), "at most 8");
        Bad(new RarityUpgradeRule { Max = (ItemRarity)7 }, "Max");
        Bad(new RarityUpgradeRule { Items = [" "] }, "Items[0]");
        ItemUpgrades.Check(Rule((ItemRarity.Uncommon, new UpgradeStep()), (ItemRarity.Legendary, new UpgradeStep { Chance = 0, Cost = { ["Shard"] = 100000 } })), "rule");
    }

    [Fact]
    public void TheLevelScaleRoundsUpAndStopsAtTheMaximum()
    {
        Assert.Equal(10, ItemUpgrades.Scaled(10, 0, 30));
        Assert.Equal(10, ItemUpgrades.Scaled(10, 0.5, 1));
        Assert.Equal(20, ItemUpgrades.Scaled(10, 0.1, 11));    // 10 x (1 + 0.1 x 10)
        Assert.Equal(12, ItemUpgrades.Scaled(10, 0.1, 3));     // 10 x 1.2 = 12
        Assert.Equal(4, ItemUpgrades.Scaled(3, 0.05, 6));      // 3 x 1.25 = 3.75 -> 4
        Assert.Equal(100000, ItemUpgrades.Scaled(50000, 10, 255));
    }

    [Fact]
    public void ARuleTakesItemsByNamePatternAndOwnMod()
    {
        static ItemId? Lookup(string n) => n switch { "Weapon_Sword" => new ItemId(1), "my-mod.ember" => new ItemId(2), _ => null };
        var all = new RarityUpgradeRule();
        Assert.True(ItemUpgrades.Takes(all, null, new ItemId(9), "Anything", Lookup));
        var weapons = new RarityUpgradeRule { Items = ["Weapon_*"] };
        Assert.True(ItemUpgrades.Takes(weapons, null, new ItemId(1), "Weapon_Sword", Lookup));
        Assert.False(ItemUpgrades.Takes(weapons, null, new ItemId(3), "Armor_Chest", Lookup));
        var own = new RarityUpgradeRule { Items = ["ember"] };
        Assert.True(ItemUpgrades.Takes(own, "my-mod", new ItemId(2), "my-mod.ember", Lookup));
        Assert.False(ItemUpgrades.Takes(own, "other", new ItemId(2), "my-mod.ember", Lookup));
        Assert.True(ItemUpgrades.Takes(new RarityUpgradeRule { Items = ["my-mod:ember"] }, null, new ItemId(2), "my-mod.ember", Lookup));
        Assert.True(ItemUpgrades.Takes(new RarityUpgradeRule { Items = ["my-mod:*"] }, null, new ItemId(2), "my-mod.ember", Lookup));
        Assert.Equal(new ItemId(2), ItemUpgrades.Resolve("ember", "my-mod", Lookup));
        Assert.Null(ItemUpgrades.Resolve("nothing", "my-mod", Lookup));
    }

    private static GearState State(int enhancements, int unlocked, int level = 5) =>
        new(ItemInstanceRef.None, new ItemId(0x1234), (ItemRarity)(enhancements - 1), ItemRarity.Common, ItemRarity.Legendary, level, level - unlocked,
            enhancements, unlocked, level + enhancements - unlocked, false, 100, 100, []) { HasEnhancementSlots = true };

    private static ItemInstanceRef Stamped(uint pide, uint item, uint stamp) =>
        new(new EntityRef(new EntityId(pide), IdentitySources.Stamp(EntitySourceOrder.Items, stamp)), new ItemId(item));

    [Fact]
    public void ASuccessWritesTheNextSlotAndPaysAFailedRollFollowsItsRule()
    {
        var item = Stamped(5000, 0x1234, 7);
        var g = State(3, 1);
        var cost = new Dictionary<ItemId, int> { [new ItemId(0xAA)] = 10, [new ItemId(0xBB)] = 2 };
        var rule = new RarityUpgradeRule();
        var player = new EntityId(3);
        var chest = new EntityId(9000);
        var ok = ItemUpgrades.RequestOf(player, chest, item, 7, g, ItemRarity.Epic, new UpgradeStep(), rule, true, cost, false)!.Value;
        Assert.Equal(ItemsNative.UpgradeWrite, ok.Flags);
        Assert.Equal((3u, 3u, 2u, 1u), (ok.Player, ok.ExpectAvailable, ok.CostCount, ok.PayerCount));
        Assert.Equal(9000u, ok.Payers[0]);
        Assert.Equal((0xAAu, 10u, 0xBBu, 2u), (ok.Costs[0], ok.Costs[1], ok.Costs[2], ok.Costs[3]));
        Assert.Equal((ItemsNative.GearAvailable | ItemsNative.GearDurabilityRefresh, (byte)4, 0x1234u), (ok.Gear.Flags, ok.Gear.Available, ok.Gear.Item));
        var unlock = ItemUpgrades.RequestOf(player, player, item, 7, g, ItemRarity.Epic, new UpgradeStep { Unlock = true }, rule, true, cost, false)!.Value;
        Assert.Equal((ItemsNative.GearAvailable | ItemsNative.GearUnlocked | ItemsNative.GearDurabilityRefresh, (byte)4, (byte)2), (unlock.Gear.Flags, unlock.Gear.Available, unlock.Gear.Unlocked));
        var lose = ItemUpgrades.RequestOf(player, player, item, 7, g, ItemRarity.Epic, new UpgradeStep(), rule, false, cost, false)!.Value;
        Assert.Equal((0u, 0u, 2u), (lose.Flags, lose.Gear.Flags, lose.CostCount));  // pay only
        Assert.Null(ItemUpgrades.RequestOf(player, player, item, 7, g, ItemRarity.Epic, new UpgradeStep { OnFail = UpgradeFail.KeepItemKeepCost }, rule, false, cost, false));
        var check = ItemUpgrades.RequestOf(player, player, item, 7, g, ItemRarity.Epic, new UpgradeStep(), rule, false, cost, false, check: true)!.Value;
        Assert.Equal((0u, 0u, 0u), (check.Flags, check.CostCount, check.PayerCount));
        var down = ItemUpgrades.RequestOf(player, player, item, 7, g, ItemRarity.Epic, new UpgradeStep { OnFail = UpgradeFail.Downgrade }, rule, false, cost, false)!.Value;
        Assert.Equal((ItemsNative.UpgradeWrite, (byte)2, 2u), (down.Flags, down.Gear.Available, down.CostCount));
        var common = ItemUpgrades.RequestOf(player, player, item, 7, State(1, 0), ItemRarity.Uncommon, new UpgradeStep { OnFail = UpgradeFail.Downgrade }, rule, false, cost, false)!.Value;
        Assert.Equal(0u, common.Flags);  // a Common item stays Common: the cost alone
        var dry = ItemUpgrades.RequestOf(player, player, item, 7, g, ItemRarity.Epic, new UpgradeStep(), rule with { Durability = DurabilityRule.Keep, AllowEmptySlot = true }, true, cost, true)!.Value;
        Assert.Equal(ItemsNative.UpgradeWrite | ItemsNative.UpgradeDryRun, dry.Flags);
        Assert.Equal(ItemsNative.GearAvailable | ItemsNative.GearForceSlot, dry.Gear.Flags);
    }

    [Fact]
    public void EveryNativeStatusHasAnUpgradeStatus()
    {
        Assert.Equal(UpgradeStatus.Done, ItemUpgrades.StatusOf(ItemsNative.GearDone, true));
        Assert.Equal(UpgradeStatus.Failed, ItemUpgrades.StatusOf(ItemsNative.GearDone, false));
        Assert.Equal(UpgradeStatus.Stale, ItemUpgrades.StatusOf(ItemsNative.GearStale, true));
        Assert.Equal(UpgradeStatus.NotGear, ItemUpgrades.StatusOf(ItemsNative.GearNotGear, true));
        Assert.Equal(UpgradeStatus.NoSlot, ItemUpgrades.StatusOf(ItemsNative.GearNoSlot, true));
        Assert.Equal(UpgradeStatus.Unavailable, ItemUpgrades.StatusOf(ItemsNative.GearUnavailableStatus, true));
        Assert.Equal(UpgradeStatus.Dropped, ItemUpgrades.StatusOf(ItemsNative.GearDropped, true));
        Assert.Equal(UpgradeStatus.CantPay, ItemUpgrades.StatusOf(ItemsNative.UpgradeCantPay, true));
        foreach (var s in new[] { ItemsNative.GearOutOfRange, ItemsNative.GearEquipped, ItemsNative.GearRefused, ItemsNative.UpgradeBadPayer, ItemsNative.UpgradeChanged, 99 })
            Assert.Equal(UpgradeStatus.Refused, ItemUpgrades.StatusOf(s, true));
    }

    [Fact]
    public void TheUpgradeRowFollowsWritesTheTableAndTheTransaction()
    {
        var on = (CapabilityState.Available, CapabilityReason.None, "");
        var tx = new Capability("items.transaction", CapabilityState.Available, CapabilityReason.None, "", "test");
        Assert.Equal(CapabilityState.Available, Items.DecideUpgrade(on, true, tx).Item1);
        var off = Items.DecideUpgrade((CapabilityState.Unavailable, CapabilityReason.Faulted, "boom"), true, tx);
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.Faulted), (off.Item1, off.Item2));
        Assert.Contains("needs items.gear.write: boom", off.Item3);
        Assert.Equal(CapabilityReason.ServiceOlder, Items.DecideUpgrade(on, false, tx).Item2);
        var noTx = Items.DecideUpgrade(on, true, tx with { State = CapabilityState.Unavailable, Reason = CapabilityReason.SymbolMissing, ReasonText = "gone" });
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.SymbolMissing), (noTx.Item1, noTx.Item2));
    }

    // ---- on the test kit's kiln.items ----

    private sealed class Run : IDisposable
    {
        public readonly KilnHarness Kiln = new();
        public readonly FakePlayer Ada;
        public readonly ItemId Sword, Scraps, Sheets;
        public readonly List<KilnEvent> Events = [];
        private readonly List<IDisposable> _subs = [];

        public Run(int perkSlots = 5)
        {
            Sword = Kiln.Items.AddItem("Weapon_T1_1H_Sword_Test", 1, ItemCategory.Weapons, gear: true, rarity: ItemRarity.Common, levelMin: 5, levelMax: 5);
            Scraps = Kiln.Items.AddItem("Material_Scraps_Test", 100);
            Sheets = Kiln.Items.AddItem("Material_Sheets_Test", 100);
            if (perkSlots != 5) Kiln.Items.SetPerkSlots(Sword, perkSlots);
            Ada = Kiln.Players.Join("Ada", AdaSteam, position: new WorldPosition(0, 0, 0));
            Kiln.Step(2);
        }

        public void Listen()
        {
            _subs.Add(Kiln.Run(() => global::Kiln.Events.On<GearRarityChangedEvent>(Events.Add)));
            _subs.Add(Kiln.Run(() => global::Kiln.Events.On<GearUpgradedEvent>(Events.Add)));
        }

        public void On<T>(Action<T> handler) where T : KilnEvent => _subs.Add(Kiln.Run(() => global::Kiln.Events.On(handler)));

        public ItemInstanceRef GiveSword(ItemRarity rarity)
        {
            var give = Kiln.Run(() => Items.Give(Ada.Entity, Sword, 1, new Gear(5, rarity)));
            Kiln.Step();
            Assert.True(give.Result.Succeeded, give.Result.ToString());
            return Assert.Single(give.Result.Changes).Ref;
        }

        public void Give(EntityId to, ItemId item, int n)
        {
            var give = Kiln.Run(() => Items.Give(to, item, n));
            Kiln.Step();
            Assert.True(give.Result.Succeeded, give.Result.ToString());
        }

        public int Count(EntityId who, ItemId item) => Kiln.Items.Of(who)!.Slots.Where(s => s.Item == item).Sum(s => s.Count);

        public RarityUpgradeRule Standard(double chance = 1, UpgradeFail onFail = UpgradeFail.KeepItemLoseCost) => new()
        {
            Items = ["Weapon_*"],
            Steps =
            {
                [ItemRarity.Uncommon] = new UpgradeStep { Cost = { ["Material_Scraps_Test"] = 10 } },
                [ItemRarity.Rare] = new UpgradeStep { Cost = { ["Material_Scraps_Test"] = 10 } },
                [ItemRarity.Epic] = new UpgradeStep { Cost = { ["Material_Scraps_Test"] = 10, ["Material_Sheets_Test"] = 2 }, Chance = chance, OnFail = onFail },
                [ItemRarity.Legendary] = new UpgradeStep { Cost = { ["Material_Sheets_Test"] = 5 } },
            },
        };

        public UpgradeResult Upgrade(Func<Task<UpgradeResult>> call)
        {
            var t = Kiln.Run(call);
            if (!t.IsCompleted) Kiln.Step();  // the upgrade runs at the start of the next step
            Assert.True(t.IsCompletedSuccessfully, t.Exception?.ToString());
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
    public void TheRowIsOnInTheKit()
    {
        using var run = new Run();
        Assert.True(run.Kiln.Run(() => Items.UpgradeAvailable), run.Kiln.Run(() => Capabilities.Why("items.gear.upgrade")));
    }

    [Fact]
    public void ArgumentsAreCheckedBeforeAnythingElse()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        var rule = run.Standard();
        run.Kiln.Run(() =>
        {
            Assert.Throws<ArgumentException>(() => Call(() => Items.UpgradeRarity(EntityId.None, item, rule)));
            Assert.Throws<ArgumentException>(() => Call(() => Items.UpgradeRarity(run.Ada.Entity, ItemInstanceRef.None, rule)));
            Assert.Throws<ArgumentNullException>(() => Call(() => Items.UpgradeRarity(run.Ada.Entity, item, (RarityUpgradeRule)null!)));
            Assert.Throws<ArgumentException>(() => Call(() => Items.UpgradeRarity(run.Ada.Entity, item, rule with { Max = (ItemRarity)9 })));
            Assert.Throws<ArgumentException>(() => Call(() => Items.UpgradeRarity(run.Ada.Entity, item, "no-such-rule")));
        });
        Assert.Throws<InvalidOperationException>(() => Call(() => Items.UpgradeRarity(run.Ada.Entity, item, rule)));  // off the tick thread
    }

    [Fact]
    public void AnUpgradePaysAndRaisesInPlaceInOneStep()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Give(run.Ada.Entity, run.Scraps, 15);
        run.Give(run.Ada.Entity, run.Sheets, 3);
        run.Listen();
        var r = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard()));
        Assert.True(r.Ok, r.ToString());
        Assert.Equal((ItemRarity.Rare, ItemRarity.Epic, 4, 0), (r.From, r.To, r.After!.Enhancements, r.After.Unlocked));
        Assert.Equal(new Dictionary<ItemId, int> { [run.Scraps] = 10, [run.Sheets] = 2 }, r.Paid);
        Assert.Null(r.Rolled);  // a chance of 1 rolls nothing
        Assert.Equal((5, 1), (run.Count(run.Ada.Entity, run.Scraps), run.Count(run.Ada.Entity, run.Sheets)));
        Assert.Equal(ItemRarity.Epic, run.State(item).Rarity);
        Assert.Equal(item.Entity.Id, run.Ada.Inventory.Slots.Single(s => s.Item == run.Sword).ItemEntity);  // the same item entity
        Assert.Equal(["GearRarityChangedEvent Upgrade", "GearUpgradedEvent"],
            run.Events.Select(e => e is GearRarityChangedEvent g ? $"{e.GetType().Name} {g.Source}" : e.GetType().Name));
        Assert.True(((GearUpgradedEvent)run.Events[1]).Succeeded);
    }

    [Fact]
    public void TooLittleToPayChangesNothing()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Give(run.Ada.Entity, run.Scraps, 15);
        run.Give(run.Ada.Entity, run.Sheets, 1);  // two needed
        run.Listen();
        var r = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard()));
        Assert.Equal(UpgradeStatus.CantPay, r.Status);
        Assert.Contains("hold 1 of the 2", r.Reason);
        Assert.Empty(r.Paid);
        Assert.Equal((15, 1), (run.Count(run.Ada.Entity, run.Scraps), run.Count(run.Ada.Entity, run.Sheets)));  // the scraps stay too
        Assert.Equal(ItemRarity.Rare, run.State(item).Rarity);
        Assert.Empty(run.Events);
    }

    [Fact]
    public void AChestPays()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Common);
        var chest = run.Kiln.Entities.Create(new WorldPosition(1, 0, 0));
        run.Kiln.Items.AddContainer(chest).Put(run.Scraps, 12);
        run.Give(run.Ada.Entity, run.Scraps, 50);
        var r = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard(), new UpgradeOptions { PayFrom = chest }));
        Assert.True(r.Ok, r.ToString());
        Assert.Equal((2, 50), (run.Count(chest, run.Scraps), run.Count(run.Ada.Entity, run.Scraps)));
        // another player is no payer
        var bob = run.Kiln.Players.Join("Bob", AdaSteam + 1, position: new WorldPosition(0, 0, 0));
        run.Kiln.Step();
        run.Give(bob.Entity, run.Scraps, 50);
        var refused = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard(), new UpgradeOptions { PayFrom = bob.Entity }));
        Assert.Equal(UpgradeStatus.Refused, refused.Status);
        Assert.Contains("another player", refused.Reason);
        Assert.Equal(50, run.Count(bob.Entity, run.Scraps));
    }

    [Fact]
    public void AFailedRollKeepsTheItemAndSpendsTheCostByDefault()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Give(run.Ada.Entity, run.Scraps, 30);
        run.Give(run.Ada.Entity, run.Sheets, 6);
        run.Listen();
        run.Kiln.Items.SetUpgradeRolls(0.75, 0.25);
        var failed = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard(chance: 0.5)));
        Assert.Equal(UpgradeStatus.Failed, failed.Status);
        Assert.Equal((0.75, 0.5), (failed.Rolled, failed.Chance));
        Assert.Equal(new Dictionary<ItemId, int> { [run.Scraps] = 10, [run.Sheets] = 2 }, failed.Paid);
        Assert.Equal((20, 4), (run.Count(run.Ada.Entity, run.Scraps), run.Count(run.Ada.Entity, run.Sheets)));
        Assert.Equal(ItemRarity.Rare, run.State(item).Rarity);
        var ok = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard(chance: 0.5)));
        Assert.True(ok.Ok, ok.ToString());
        Assert.Equal(0.25, ok.Rolled);
        Assert.Equal(["GearUpgradedEvent", "GearRarityChangedEvent", "GearUpgradedEvent"], run.Events.Select(e => e.GetType().Name));
        Assert.False(((GearUpgradedEvent)run.Events[0]).Succeeded);
    }

    [Fact]
    public void KeepItemKeepCostPaysNothingAndDowngradeLowersARarity()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Rare);
        run.Give(run.Ada.Entity, run.Scraps, 30);
        run.Give(run.Ada.Entity, run.Sheets, 6);
        run.Kiln.Items.SetUpgradeRolls(0.9, 0.9);
        var kept = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard(0.5, UpgradeFail.KeepItemKeepCost)));
        Assert.Equal(UpgradeStatus.Failed, kept.Status);
        Assert.Empty(kept.Paid);
        Assert.Equal((30, 6), (run.Count(run.Ada.Entity, run.Scraps), run.Count(run.Ada.Entity, run.Sheets)));
        var down = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard(0.5, UpgradeFail.Downgrade)));
        Assert.Equal(UpgradeStatus.Failed, down.Status);
        Assert.Equal((ItemRarity.Rare, ItemRarity.Uncommon), (down.Before!.Rarity, down.After!.Rarity));
        Assert.Equal((20, 4), (run.Count(run.Ada.Entity, run.Scraps), run.Count(run.Ada.Entity, run.Sheets)));
    }

    [Fact]
    public void RefusalsPayNothing()
    {
        using var run = new Run(perkSlots: 3);
        var item = run.GiveSword(ItemRarity.Rare);
        run.Give(run.Ada.Entity, run.Scraps, 30);
        run.Give(run.Ada.Entity, run.Sheets, 30);
        var noSlot = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard()));
        Assert.Equal(UpgradeStatus.NoSlot, noSlot.Status);
        var atMax = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard() with { Max = ItemRarity.Rare }));
        Assert.Equal(UpgradeStatus.AtMax, atMax.Status);
        var other = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard() with { Items = ["Armor_*"] }));
        Assert.Equal(UpgradeStatus.NotAllowed, other.Status);
        var noStep = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, new RarityUpgradeRule { AllowEmptySlot = true }));
        Assert.Equal(UpgradeStatus.NotAllowed, noStep.Status);
        Assert.Contains("no step to Epic", noStep.Reason);
        var empty = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard() with { AllowEmptySlot = true }));
        Assert.True(empty.Ok, empty.ToString());  // the empty slot allowed
        Assert.Equal((20, 28), (run.Count(run.Ada.Entity, run.Scraps), run.Count(run.Ada.Entity, run.Sheets)));
    }

    [Fact]
    public void AHandlerCancelsOrChangesThePriceAndTheChance()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Common);
        run.Give(run.Ada.Entity, run.Scraps, 30);
        var cancel = true;
        run.On<GearUpgradingEvent>(e =>
        {
            Assert.Equal((ItemRarity.Common, ItemRarity.Uncommon, 10), (e.From, e.To, e.Cost[run.Scraps]));
            if (cancel) e.Cancel();
            else
            {
                e.Cost = new Dictionary<ItemId, int> { [run.Scraps] = 3 };
                e.Chance = 0.1;
            }
        });
        var canceled = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard()));
        Assert.Equal(UpgradeStatus.Canceled, canceled.Status);
        Assert.Equal(30, run.Count(run.Ada.Entity, run.Scraps));
        cancel = false;
        run.Kiln.Items.SetUpgradeRolls(0.05);
        var cheap = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard()));
        Assert.True(cheap.Ok, cheap.ToString());
        Assert.Equal((0.1, 0.05), (cheap.Chance, cheap.Rolled));
        Assert.Equal(27, run.Count(run.Ada.Entity, run.Scraps));
    }

    [Fact]
    public void UnlockLevelScaleFreeAndDryRun()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Common);
        run.Give(run.Ada.Entity, run.Scraps, 100);
        var scaled = new RarityUpgradeRule { Steps = { [ItemRarity.Uncommon] = new UpgradeStep { Cost = { ["Material_Scraps_Test"] = 10 }, LevelScale = 0.25, Unlock = true } } };
        var dry = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, scaled, new UpgradeOptions { DryRun = true }));
        Assert.True(dry.Ok && dry.DryRun, dry.ToString());
        Assert.Equal(20, dry.Paid[run.Scraps]);  // 10 x (1 + 0.25 x (5 - 1))
        Assert.Equal((ItemRarity.Uncommon, 1), (dry.After!.Rarity, dry.After.Unlocked));
        Assert.Equal((100, ItemRarity.Common), (run.Count(run.Ada.Entity, run.Scraps), run.State(item).Rarity));
        var r = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, scaled));
        Assert.True(r.Ok, r.ToString());
        Assert.Equal((2, 1, 6), (r.After!.Enhancements, r.After.Unlocked, r.After.Level));  // the new slot unlocked: one level more
        Assert.Equal(80, run.Count(run.Ada.Entity, run.Scraps));
        var free = run.Upgrade(() => Items.UpgradeRarity(run.Ada.Entity, item, run.Standard(), new UpgradeOptions { Free = true }));
        Assert.True(free.Ok, free.ToString());
        Assert.Empty(free.Paid);
        Assert.Equal(80, run.Count(run.Ada.Entity, run.Scraps));
    }

    [Fact]
    public void ARuleByNameAndTwoUpgradesInOneStep()
    {
        using var run = new Run();
        var item = run.GiveSword(ItemRarity.Common);
        run.Give(run.Ada.Entity, run.Scraps, 100);
        ItemUpgrades.Register("test-mod:standard", run.Standard() with { Id = "test-mod:standard", Mod = "test-mod" });
        try
        {
            var first = run.Kiln.Run(() => Items.UpgradeRarity(run.Ada.Entity, item, "test-mod:standard"));
            var second = run.Kiln.Run(() => Items.UpgradeRarity(run.Ada.Entity, item, "test-mod:standard"));
            run.Kiln.Step();
            Assert.True(first.Result.Ok, first.Result.ToString());
            Assert.Equal("test-mod:standard", first.Result.Rule);
            Assert.Equal(UpgradeStatus.Refused, second.Result.Status);  // priced for Common; the first made it Uncommon
            Assert.Contains("priced for", second.Result.Reason);
            Assert.Equal(90, run.Count(run.Ada.Entity, run.Scraps));  // paid once
            Assert.Equal(ItemRarity.Uncommon, run.State(item).Rarity);
        }
        finally
        {
            ItemUpgrades.Forget("test-mod");
        }
        Assert.Null(ItemUpgrades.Find("test-mod:standard", null));
    }
}
