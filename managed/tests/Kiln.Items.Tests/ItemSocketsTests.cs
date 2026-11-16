// SPDX-License-Identifier: MIT
// Gem wave G4 offline: Kiln.Items' part of sockets on any gear (ItemSockets): the kinds as kiln.items' createPide hook
// classifies them, the allowance per mod, the re-creating request of Gems.AddSocket and the v7 mirrors' sizes. What the
// game does is the lab test (mods/tests/gems-sockets-test); the native logic is native/services/items/tests (pide_test,
// move_test replace).
using System.Runtime.InteropServices;
using Kiln;
using Xunit;

namespace Kiln.ItemsTests;

public sealed unsafe class ItemSocketsTests
{
    [Fact]
    public void TheV7MirrorsMatchTheHeader()
    {
        Assert.Equal(40, sizeof(ItemsNative.Request));
        Assert.Equal(56, sizeof(ItemsNative.RequestEx));
        Assert.Equal(40, (int)Marshal.OffsetOf<ItemsNative.RequestEx>(nameof(ItemsNative.RequestEx.DyeItem)));
        Assert.Equal(44, (int)Marshal.OffsetOf<ItemsNative.RequestEx>(nameof(ItemsNative.RequestEx.Durability)));
        Assert.Equal(24, sizeof(ItemsNative.SocketRoll));
        Assert.Equal(sizeof(ItemsNative.TableV6) + 8, sizeof(ItemsNative.TableV7));
        Assert.Equal((0x400u, 0x800u, 0x1000u, 0x2000u),
            (ItemsNative.GemSlotAny, ItemsNative.Replace, ItemsNative.Dye, ItemsNative.DurabilityFlag));
    }

    [Theory]
    [InlineData(ItemCategory.Equipment, 37, ItemSockets.Rings)]
    [InlineData(ItemCategory.Equipment, 13, ItemSockets.Armour)]
    [InlineData(ItemCategory.Equipment, 17, ItemSockets.Armour)]
    [InlineData(ItemCategory.Equipment, 18, ItemSockets.Shields)]
    [InlineData(ItemCategory.Equipment, 38, ItemSockets.Other)]   // backpacks
    [InlineData(ItemCategory.Tools, 2, ItemSockets.Other)]
    [InlineData(ItemCategory.Weapons, 1, 0u)]                     // the game's own sockets
    [InlineData(ItemCategory.WeaponGem, 0, 0u)]
    [InlineData(ItemCategory.ColorPalette, 0, 0u)]
    [InlineData(ItemCategory.Customization, 19, 0u)]
    public void KindsAreTheNativeHooksKinds(ItemCategory category, byte slot, uint kind) =>
        Assert.Equal(kind, ItemSockets.KindOf(category, slot));

    [Fact]
    public void KilnsOwnAllowanceIsTheUnionAndEnds()
    {
        try
        {
            ItemSockets.SetAllowed(null, ItemSockets.Rings | ItemSockets.Shields);
            Assert.Equal(ItemSockets.Rings | ItemSockets.Shields, ItemSockets.AllowedKinds);
            ItemSockets.SetAllowed(null, 0);
            Assert.Equal(0u, ItemSockets.AllowedKinds);
            ItemSockets.SetAllowed(null, 0xFF);
            Assert.Equal(ItemSockets.AllKinds, ItemSockets.AllowedKinds);  // unknown bits never reach the union
        }
        finally { ItemSockets.SetAllowed(null, 0); }
    }

    private static ItemSockets.GearState Ring(int level = 34, int unlocked = 4, int dur = 300, int durMax = 1200, uint dye = 0) =>
        new(Found: true, Placed: true, Equipped: false, Item: new ItemId(18908173), Holder: new EntityId(2), Slot: new SlotId(0x3_0000_1234),
            HasSocket: false, Gem: ItemId.None, GemLevel: 0, Level: level, HasDurability: durMax > 0, Durability: dur,
            DurabilityMax: durMax, Dye: new ItemId(dye), PerksAvailable: 4, PerksUnlocked: unlocked);

    [Fact]
    public void ARecreationAsksForTheSameItemAtItsBaseLevelWithItsUpgradesDyeAndDurability()
    {
        var rx = ItemSockets.RecreateRequest(new EntityId(2), Ring(dye: 0x1234), ItemRarity.Epic, socket: true);
        Assert.Equal((uint)sizeof(ItemsNative.RequestEx), rx.Base.Size);
        Assert.Equal(2u, rx.Base.Entity);
        Assert.Equal(18908173u, rx.Base.Item);
        Assert.Equal(1u, rx.Base.Count);
        Assert.Equal(0x3_0000_1234ul, rx.Base.Slot);
        Assert.Equal(30u, rx.Base.Level);       // 34 with 4 upgrades: the give unlocks them again and adds 4
        Assert.Equal(4, rx.Base.Unlock);
        Assert.Equal((byte)ItemRarity.Epic, rx.Base.Rarity);
        Assert.Equal(0x1234u, rx.DyeItem);
        Assert.Equal(0.25f, rx.Durability);
        const uint want = ItemsNative.Replace | ItemsNative.AllOrNothing | ItemsNative.Level | ItemsNative.Rarity | ItemsNative.Unlock |
                          ItemsNative.GemSlot | ItemsNative.GemSlotAny | ItemsNative.Dye | ItemsNative.DurabilityFlag;
        Assert.Equal(want, rx.Base.Flags);
        Assert.Equal(0u, rx.R0 | rx.R1);
    }

    [Fact]
    public void ARecreationLeavesOutWhatTheItemDoesNotHave()
    {
        var rx = ItemSockets.RecreateRequest(new EntityId(2), Ring(level: 12, unlocked: 0, durMax: 0), null, socket: true);
        Assert.Equal(ItemsNative.Replace | ItemsNative.AllOrNothing | ItemsNative.Level | ItemsNative.GemSlot | ItemsNative.GemSlotAny,
            rx.Base.Flags);
        Assert.Equal(12u, rx.Base.Level);
        Assert.Equal(0, rx.Base.Unlock);
        var all = ItemSockets.RecreateRequest(new EntityId(2), Ring(level: 55, unlocked: 5), ItemRarity.Legendary, socket: false);
        Assert.Equal(255, all.Base.Unlock);     // 5 upgrades: "all of them"
        Assert.Equal(50u, all.Base.Level);
        Assert.Equal(0u, all.Base.Flags & (ItemsNative.GemSlot | ItemsNative.GemSlotAny));
        var broken = ItemSockets.RecreateRequest(new EntityId(2), Ring(dur: 0), null, socket: true);
        Assert.Equal(0f, broken.Durability);    // a broken item stays broken (no free repair)
    }
}
