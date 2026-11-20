// SPDX-License-Identifier: MIT
// One item's values beyond its game components (the equipment wave, package P7) offline: Items.SetPower, SetArmor,
// ValuesOf, SetDurabilityMax and SetLevel with AnyLevel on the test kit's kiln.items (its gear model ports
// native/services/items/gear_logic.h; its scale table the request checks of scale_logic.h), the requests they send, and
// what Kiln keeps through loads through the keeper Kiln.Blocks registers (here a fake keeper). What the game does with a
// scale (the damage of a swing, the armour of a worn piece) is the guided client checks equip-p7.power and equip-p7.armor.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

[Collection("gear keeper")]
public sealed unsafe class GearValuesTests : IDisposable
{
    private const ulong AdaSteam = 76561198000000902;

    private sealed class FakeKeeper : IGearKeeper
    {
        public readonly Dictionary<ItemInstanceRef, GearKept> Kept = [];
        public bool Refuse;
        public GearKept? Of(ItemInstanceRef item) => Kept.GetValueOrDefault(item);

        public bool Store(ItemInstanceRef item, GearKept? kept)
        {
            if (Refuse) return false;
            if (kept is null) Kept.Remove(item);
            else Kept[item] = kept;
            return true;
        }
    }

    private readonly KilnHarness _kiln = new();
    private readonly FakePlayer _ada;
    private readonly ItemId _sword;
    private readonly FakeKeeper _keeper = new();
    private readonly IGearKeeper? _before = GearKeep.Keeper;

    public GearValuesTests()
    {
        _sword = _kiln.Items.AddItem("Weapon_T1_1H_Sword_Values", 1, ItemCategory.Weapons, gear: true, rarity: ItemRarity.Common, levelMin: 5, levelMax: 10);
        _ada = _kiln.Players.Join("Ada", AdaSteam, position: new WorldPosition(0, 0, 0));
        _kiln.Step(2);
        GearKeep.Keeper = _keeper;
    }

    public void Dispose()
    {
        GearKeep.Keeper = _before;
        _kiln.Dispose();
    }

    private ItemInstanceRef GiveSword(int level = 5)
    {
        var give = _kiln.Run(() => Items.Give(_ada.Entity, _sword, 1, new Gear(level, ItemRarity.Rare)));
        _kiln.Step();
        Assert.True(give.Result.Succeeded, give.Result.ToString());
        return Assert.Single(give.Result.Changes).Ref;
    }

    // The calls return a task but throw before one exists: run them as a plain call.
    private static void Call(Func<Task> f) => _ = f();

    private GearResult Write(Func<Task<GearResult>> call)
    {
        var t = _kiln.Run(call);
        _kiln.Step();
        Assert.True(t.IsCompletedSuccessfully);
        return t.Result;
    }

    // ---- the ABI and the requests ----

    [Fact]
    public void TheMirrorsHaveTheHeadersSizes()
    {
        Assert.Equal(32, sizeof(ItemsNative.GearScale));   // native items.cpp: sizeof(KilnItemsGearScale) == 32
        Assert.Equal(208, sizeof(ItemsNative.ScaleStatus)); // sizeof(KilnItemsScaleStatus) == 208
        Assert.Equal(344, (int)System.Runtime.InteropServices.Marshal.OffsetOf<ItemsNative.TableV8>(nameof(ItemsNative.TableV8.GearScaleSet)));
        Assert.Equal((int)ItemsNative.V8ScaleEnd, sizeof(ItemsNative.TableV8));
        Assert.Equal(28, (int)System.Runtime.InteropServices.Marshal.OffsetOf<ItemsNative.GearRequest>(nameof(ItemsNative.GearRequest.DurabilityMax)));
    }

    [Fact]
    public void AnyLevelAndAMaxDurabilitySendTheirFlags()
    {
        var item = new ItemInstanceRef(new EntityRef(new EntityId(5000), IdentitySources.Stamp(EntitySourceOrder.Items, 7)), new ItemId(0x1234));
        var l = Items.GearRequestOf(item, 7, Items.GearChange.ToLevel(60), new GearWriteOptions { AnyLevel = true, Durability = DurabilityRule.Keep });
        Assert.Equal((ItemsNative.GearLevel | ItemsNative.GearAnyLevel, 60u), (l.Flags, l.Level));
        var m = Items.GearRequestOf(item, 7, new Items.GearChange(null, null, null, 900), GearWriteOptions.Default);
        Assert.Equal((ItemsNative.GearDurabilityMax, 900), (m.Flags, m.DurabilityMax));  // KeepRatio's refresh would replace the max: not sent
        var full = Items.GearRequestOf(item, 7, new Items.GearChange(null, null, null, 900), new GearWriteOptions { Durability = DurabilityRule.Repair });
        Assert.Equal(ItemsNative.GearDurabilityMax | ItemsNative.GearDurabilityRepair, full.Flags);
        Assert.Throws<ArgumentException>(() => Items.GearRequestOf(item, 7, Items.GearChange.ToRarity(ItemRarity.Epic), new GearWriteOptions { AnyLevel = true }));
        Assert.Throws<ArgumentOutOfRangeException>(() => Items.GearRequestOf(item, 7, new Items.GearChange(null, null, null, 0), GearWriteOptions.Default));
    }

    // ---- the whole path on the kit ----

    [Fact]
    public void SetPowerAndSetArmorAreKeptPerItemAndOneGivesTheItemsOwnBack()
    {
        var sword = GiveSword();
        var other = GiveSword();
        Assert.True(_kiln.Run(() => Items.PowerAvailable && Items.ArmorAvailable), _kiln.Run(() => Capabilities.Why("items.gear.power")));
        var r = _kiln.Run(() => Items.SetPower(sword, 1.5));
        Assert.True(r.Ok, r.ToString());
        Assert.Equal((1.5, 1.0, true), (r.Power, r.Armor, r.Kept));
        var a = _kiln.Run(() => Items.SetArmor(sword, 0.5));
        Assert.Equal((1.5, 0.5), (a.Power, a.Armor));
        var v = _kiln.Run(() => Items.ValuesOf(sword))!;
        Assert.Equal((1.5, 0.5, null, null, true), (v.Power, v.Armor, v.KeptBaseLevel, v.KeptDurabilityMax, v.Kept));
        Assert.Equal((1.0, 1.0), (_kiln.Run(() => Items.ValuesOf(other))!.Power, _kiln.Run(() => Items.ValuesOf(other))!.Armor));  // only this item
        Assert.Equal(new GearKept(1.5, 0.5), _keeper.Kept[sword]);
        _kiln.Run(() => Items.SetPower(sword, 1));
        var back = _kiln.Run(() => Items.SetArmor(sword, 1));
        Assert.Equal((1.0, 1.0, false), (back.Power, back.Armor, back.Kept));
        Assert.False(_keeper.Kept.ContainsKey(sword));  // nothing left to keep
        Assert.Equal((1.0, 1.0), (_kiln.Run(() => Items.ValuesOf(sword))!.Power, _kiln.Run(() => Items.ValuesOf(sword))!.Armor));
    }

    [Fact]
    public void ScalesAreCheckedAndAStaleItemIsAResult()
    {
        var sword = GiveSword();
        Assert.Throws<ArgumentOutOfRangeException>(() => _kiln.Run(() => Items.SetPower(sword, 0)));
        Assert.Throws<ArgumentOutOfRangeException>(() => _kiln.Run(() => Items.SetPower(sword, 101)));
        Assert.Throws<ArgumentOutOfRangeException>(() => _kiln.Run(() => Items.SetArmor(sword, double.NaN)));
        Assert.Throws<ArgumentException>(() => _kiln.Run(() => Items.SetPower(ItemInstanceRef.None, 2)));
        var stale = sword with { Entity = new EntityRef(sword.Entity.Id, IdentitySources.Stamp(EntitySourceOrder.Items, 999999)) };
        var r = _kiln.Run(() => Items.SetPower(stale, 2));
        Assert.Equal(GearValuesStatus.Stale, r.Status);
        Assert.False(r.Ok);
        Assert.Null(_kiln.Run(() => Items.ValuesOf(stale)));
        Assert.Throws<InvalidOperationException>(() => Items.SetPower(sword, 2));  // off the tick thread
    }

    [Fact]
    public void ASwitchedOffFeatureThrowsNamingItsRow()
    {
        var sword = GiveSword();
        _kiln.Capabilities.Override("items.gear.power", false, "test: off");
        var e = Assert.Throws<KilnUnavailableException>(() => _kiln.Run(() => Items.SetPower(sword, 2)));
        Assert.Equal("items.gear.power", e.Capability);
        Assert.True(_kiln.Run(() => Items.SetArmor(sword, 2)).Ok);  // the other kind is its own row
    }

    [Fact]
    public void ALevelOutsideTheRangeNeedsAnyLevelAndIsKeptAsItsBase()
    {
        var sword = GiveSword();
        var refused = Write(() => Items.SetLevel(sword, 30));
        Assert.Equal(GearStatus.OutOfRange, refused.Status);
        var r = Write(() => Items.SetLevel(sword, 30, new GearWriteOptions { AnyLevel = true }));
        Assert.True(r.Ok, r.ToString());
        Assert.True(r.Kept);
        Assert.Equal((30, 30), (r.After!.Level, r.After.BaseLevel));
        Assert.Equal(30, _keeper.Kept[sword].BaseLevel);
        Assert.Equal(30, _kiln.Run(() => Items.ValuesOf(sword))!.KeptBaseLevel);
        var back = Write(() => Items.SetLevel(sword, 7));  // inside the range again: the game keeps it, Kiln keeps nothing
        Assert.True(back.Ok, back.ToString());
        Assert.False(back.Kept);
        Assert.False(_keeper.Kept.ContainsKey(sword));
    }

    [Fact]
    public void AMaxDurabilityKeepsTheShareAndIsKeptUntilNull()
    {
        var sword = GiveSword();
        _kiln.Items.SetGear(sword, durability: 40);  // 40 / 100
        var r = Write(() => Items.SetDurabilityMax(sword, 250));
        Assert.True(r.Ok, r.ToString());
        Assert.Equal(GearChanges.DurabilityMax | GearChanges.Durability, r.Changed);
        Assert.Equal((100, 250, 100), (r.Before!.DurabilityMax, r.After!.DurabilityMax, r.After.Durability));
        Assert.True(r.Kept);
        Assert.Equal(250, _keeper.Kept[sword].DurabilityMax);
        var full = Write(() => Items.SetDurabilityMax(sword, 300, new GearWriteOptions { Durability = DurabilityRule.Repair }));
        Assert.Equal((300, 300), (full.After!.DurabilityMax, full.After.Durability));
        var cleared = Write(() => Items.SetDurabilityMax(sword, null));
        Assert.True(cleared.Ok, cleared.ToString());
        Assert.Equal(GearChanges.DurabilityRefresh, cleared.Changed);  // the game computes its own max again
        Assert.False(_keeper.Kept.ContainsKey(sword));
        _kiln.Run(() => Assert.Throws<ArgumentOutOfRangeException>(() => Call(() => Items.SetDurabilityMax(sword, 0))));
        _kiln.Run(() => Assert.Throws<ArgumentOutOfRangeException>(() => Call(() => Items.SetDurabilityMax(sword, 1000001))));
    }

    [Fact]
    public void WithoutAKeeperTheValuesActForTheSessionAndSaySo()
    {
        GearKeep.Keeper = null;
        var sword = GiveSword();
        var r = _kiln.Run(() => Items.SetPower(sword, 2));
        Assert.Equal((true, false), (r.Ok, r.Kept));
        var l = Write(() => Items.SetLevel(sword, 40, new GearWriteOptions { AnyLevel = true }));
        Assert.Equal((true, false), (l.Ok, l.Kept));
        Assert.False(_kiln.Run(() => Items.ValuesOf(sword))!.Kept);
        _keeper.Refuse = true;
        GearKeep.Keeper = _keeper;
        Assert.False(_kiln.Run(() => Items.SetArmor(sword, 2)).Kept);  // item data off: not kept
    }
}
