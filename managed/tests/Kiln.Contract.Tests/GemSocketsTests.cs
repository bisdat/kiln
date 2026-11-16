// SPDX-License-Identifier: MIT
// Gem wave G4 offline: the parts of Gems' sockets on other gear (GemSockets.cs) that decide without the game: what
// AddSocket refuses before it asks kiln.items, how a kiln.items result becomes a GemSocketResult, the forge lists'
// category mask per kind, and AllowSockets' argument checks. The native halves are tested in
// native/services/items/tests (pide_test, move_test replace) and native/core/tests (ui values sockets); the game's side
// is the lab test mods/tests/gems-sockets-test and the client checklist in docs/kiln/services/gems-sockets.md.
using Xunit;

namespace Kiln.Contract.Tests;

public sealed class GemSocketsTests
{
    private static ItemSockets.GearState Gear(bool found = true, bool placed = true, bool equipped = false, bool socket = false,
        uint gem = 0) =>
        new(found, placed, equipped, new ItemId(18908173), new EntityId(2), new SlotId(0x3_0000_1234), socket, new ItemId(gem), 0,
            34, true, 600, 1200, ItemId.None, 4, 4);

    private const uint Rings = ItemSockets.Rings, AllKinds = ItemSockets.AllKinds;

    [Fact]
    public void AnAllowedRingInABackpackGoesAhead() =>
        Assert.Null(Gems.DecideAddSocket(Gear(), ItemCategory.Equipment, 37, Rings));

    [Fact]
    public void AWeaponNeedsNoAllowance() =>
        Assert.Null(Gems.DecideAddSocket(Gear(), ItemCategory.Weapons, 1, 0));

    [Theory]
    [InlineData(false, true, false, false, 0u, GemSocketStatus.NotFound)]
    [InlineData(true, false, false, false, 0u, GemSocketStatus.NotFound)]
    [InlineData(true, true, true, false, 0u, GemSocketStatus.Equipped)]
    [InlineData(true, true, false, true, 0u, GemSocketStatus.AlreadyHasSocket)]
    [InlineData(true, true, false, true, 0x5aul, GemSocketStatus.AlreadyHasSocket)]
    public void WhatTheItemIsDecidesFirst(bool found, bool placed, bool equipped, bool socket, uint gem, GemSocketStatus want)
    {
        var d = Gems.DecideAddSocket(Gear(found, placed, equipped, socket, gem), ItemCategory.Equipment, 37, AllKinds);
        Assert.NotNull(d);
        Assert.Equal(want, d.Value.Status);
    }

    [Fact]
    public void AKindNoModAllowedIsRefusedWithTheCallThatAllowsIt()
    {
        var d = Gems.DecideAddSocket(Gear(), ItemCategory.Equipment, 14, Rings);
        Assert.Equal(GemSocketStatus.KindNotAllowed, d!.Value.Status);
        Assert.Contains("Gems.AllowSockets(GemSocketKinds.Armour)", d.Value.Reason);
    }

    [Theory]
    [InlineData(ItemCategory.WeaponGem)]
    [InlineData(ItemCategory.ColorPalette)]
    [InlineData(ItemCategory.Customization)]
    public void GemsDyesAndCustomizationNeverHoldAGem(ItemCategory category) =>
        Assert.Equal(GemSocketStatus.NotGear, Gems.DecideAddSocket(Gear(), category, 0, AllKinds)!.Value.Status);

    [Fact]
    public void AnItemTheCatalogDoesNotKnowIsNotFound() =>
        Assert.Equal(GemSocketStatus.NotFound, Gems.DecideAddSocket(Gear(), null, 0, AllKinds)!.Value.Status);

    [Fact]
    public void KilnItemsResultsMapToSocketResults()
    {
        var before = new ItemInstanceRef(new EntityRef(new EntityId(5000), 7), new ItemId(18908173));
        var after = new ItemInstanceRef(new EntityRef(new EntityId(5100), 9), new ItemId(18908173));
        var done = new ItemsResult(ItemsStatus.Done, new EntityId(2), new ItemId(18908173), 1, 1,
            [new ItemChange(new SlotId(0x3_0000_1234), new ItemId(18908173), 1, new EntityId(5100)) { Ref = after }], "", 0, 1, false);
        var r = Gems.ResultOf(before, done);
        Assert.Equal((GemSocketStatus.Done, before, after, ""), (r.Status, r.Before, r.After, r.Reason));
        Assert.True(r.Succeeded);
        var changed = ItemsResult.Fail(ItemsStatus.SlotChanged, new EntityId(2), new ItemId(18908173), 1, "the slot holds item entity 6, not 5000");
        Assert.Equal((GemSocketStatus.NotFound, before), (Gems.ResultOf(before, changed).Status, Gems.ResultOf(before, changed).After));
        Assert.Equal(GemSocketStatus.Refused, Gems.ResultOf(before, ItemsResult.Fail(ItemsStatus.Refused, default, default, 1, "x")).Status);
        Assert.Equal(GemSocketStatus.Refused, Gems.ResultOf(before, ItemsResult.Fail(ItemsStatus.NoRoom, default, default, 1, "x")).Status);
        Assert.Equal(GemSocketStatus.Unavailable, Gems.ResultOf(before, ItemsResult.Fail(ItemsStatus.Unavailable, default, default, 1, "x")).Status);
        Assert.Equal(GemSocketStatus.Unavailable, Gems.ResultOf(before, ItemsResult.Fail(ItemsStatus.Dropped, default, default, 1, "x")).Status);
    }

    // Gem wave integration: kiln.items' Done for a re-creation says the transaction ran, not that the createPide hook
    // gave the new item its socket and its unlocked upgrades (a full override list, an unknown ItemInfo layout, a fault:
    // the item is then created as the game made it). AddSocket reads the new item back: Done only when nothing is missing.
    private static readonly ItemInstanceRef OldRing = new(new EntityRef(new EntityId(5000), 7), new ItemId(18908173));
    private static readonly ItemInstanceRef NewRing = new(new EntityRef(new EntityId(5100), 9), new ItemId(18908173));
    private static readonly GemSocketResult Recreated = new(GemSocketStatus.Done, OldRing, NewRing, "");

    private static ItemSockets.GearState Read(bool socket = true, int level = 34, int unlocked = 4, uint dye = 0, bool durability = true,
        int current = 600, int max = 1200, bool found = true) =>
        new(found, true, false, new ItemId(18908173), new EntityId(2), new SlotId(0x3_0000_1234), socket, ItemId.None, 0, level, durability,
            current, max, dye == 0 ? ItemId.None : new ItemId(dye), 4, unlocked);

    [Fact]
    public void ARecreatedItemWithEverythingKeptIsDone()
    {
        var before = Read(socket: false, dye: 0x77);
        var r = Gems.CheckRecreated(Recreated, before, ItemRarity.Epic, Read(dye: 0x77), ItemRarity.Epic, null);
        Assert.Equal((GemSocketStatus.Done, OldRing, NewRing, ""), (r.Status, r.Before, r.After, r.Reason));
        Assert.Empty(r.Missing);
        Assert.True(r.Succeeded);
    }

    [Fact]
    public void ARecreatedItemWithoutItsSocketIsIncompleteAndSaysSo()
    {
        var r = Gems.CheckRecreated(Recreated, Read(socket: false), ItemRarity.Epic, Read(socket: false), ItemRarity.Epic, null);
        Assert.Equal(GemSocketStatus.Incomplete, r.Status);
        Assert.False(r.Succeeded);
        Assert.Equal(NewRing, r.After);  // the old item is gone either way: After is the item there now
        Assert.Equal(["a socket"], r.Missing);
        Assert.Contains("a socket", r.Reason);
        Assert.Contains("Incomplete", r.ToString());
    }

    [Fact]
    public void LostUpgradesLevelRarityDyeAndDurabilityAreEachNamed()
    {
        var before = Read(socket: false, level: 34, unlocked: 4, dye: 0x77, current: 600, max: 1200);
        var after = Read(level: 30, unlocked: 0, dye: 0, current: 1200, max: 1200);
        var r = Gems.CheckRecreated(Recreated, before, ItemRarity.Epic, after, ItemRarity.Common, null);
        Assert.Equal(GemSocketStatus.Incomplete, r.Status);
        Assert.Equal(5, r.Missing.Count);
        Assert.Contains(r.Missing, m => m.StartsWith("level 34", StringComparison.Ordinal) && m.Contains("30"));
        Assert.Contains(r.Missing, m => m.StartsWith("4 unlocked upgrade", StringComparison.Ordinal) && m.Contains('0'));
        Assert.Contains(r.Missing, m => m.StartsWith("rarity Epic", StringComparison.Ordinal) && m.Contains("Common"));
        Assert.Contains(r.Missing, m => m.StartsWith("dye", StringComparison.Ordinal));
        Assert.Contains(r.Missing, m => m.StartsWith("durability 50%", StringComparison.Ordinal) && m.Contains("100%"));
    }

    [Fact]
    public void WhatCannotBeReadBackIsNotCountedAsMissing()
    {
        // no Level component, no rarity in the slot, durability unreadable on the new item (the lab's reads came back
        // empty): nothing to compare, nothing claimed
        var before = Read(socket: false, level: 0, unlocked: 0, durability: true);
        var after = Read(level: 0, unlocked: 0, durability: false, current: 0, max: 0);
        var r = Gems.CheckRecreated(Recreated, before, null, after, ItemRarity.Rare, null);
        Assert.Equal(GemSocketStatus.Done, r.Status);
        Assert.Empty(r.Missing);
        // a durability within a point of the fraction is kept
        Assert.Empty(Gems.CheckRecreated(Recreated, Read(socket: false, current: 600, max: 1200), null, Read(current: 601, max: 1200), null, null).Missing);
    }

    [Fact]
    public void ANewItemThatCannotBeReadBackIsIncomplete()
    {
        var r = Gems.CheckRecreated(Recreated, Read(socket: false), null, null, null, "item instances are off");
        Assert.Equal(GemSocketStatus.Incomplete, r.Status);
        Assert.Equal(NewRing, r.After);
        Assert.Contains("item instances are off", Assert.Single(r.Missing));
        var gone = Gems.CheckRecreated(Recreated, Read(socket: false), null, Read(found: false), null, null);
        Assert.Equal(GemSocketStatus.Incomplete, gone.Status);
        Assert.Single(gone.Missing);
    }

    [Fact]
    public void ARefusalIsPassedThroughUnchecked()
    {
        var refused = new GemSocketResult(GemSocketStatus.Refused, OldRing, OldRing, "the game said no");
        Assert.Same(refused, Gems.CheckRecreated(refused, Read(socket: false), null, null, null, null));
    }

    [Fact]
    public void TheForgeListsWidenToTheCategoriesOfTheKinds()
    {
        Assert.Equal(0u, Gems.ForgeCategoryMask(GemSocketKinds.None));
        Assert.Equal(1u << 1, Gems.ForgeCategoryMask(GemSocketKinds.Rings));
        Assert.Equal(1u << 1, Gems.ForgeCategoryMask(GemSocketKinds.Armour | GemSocketKinds.Shields));
        Assert.Equal((1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8), Gems.ForgeCategoryMask(GemSocketKinds.OtherGear));
        Assert.Equal(0u, Gems.ForgeCategoryMask(GemSocketKinds.All) & ((1u << 2) | (1u << 15)));  // weapons are the game's; gems never
        Assert.Equal(0u, Gems.ForgeCategoryMask(GemSocketKinds.All) & ~0x3fffu);                // what kiln.ui.values accepts
    }

    [Fact]
    public void AllowSocketsChecksItsArguments()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => Gems.AllowSockets((GemSocketKinds)16));
        Assert.Throws<ArgumentOutOfRangeException>(() => Gems.AllowSockets(GemSocketKinds.Rings, 1.5));
        Assert.Throws<ArgumentOutOfRangeException>(() => Gems.AllowSockets(GemSocketKinds.Rings, -0.1));
        Assert.Throws<ArgumentOutOfRangeException>(() => Gems.AllowSockets(GemSocketKinds.Rings, double.NaN));
    }

    // Review: the allowance, the rolls and the forge mask belong to the mod on the calling thread (ModContext.CurrentMod,
    // tick thread only). From another thread there is none: they would become Kiln's own and outlive the mod. So the
    // call is tick-thread only (the KILN101 analyzer warns a mod that calls it elsewhere), not [AnyThread].
    [Fact]
    public void AllowSocketsIsTickThreadOnlyBecauseItsLifetimeIsTheCallingMods()
    {
        var m = typeof(Gems).GetMethod(nameof(Gems.AllowSockets))!;
        Assert.NotNull(m.GetCustomAttributes(typeof(TickThreadOnlyAttribute), false).SingleOrDefault());
        Assert.Empty(m.GetCustomAttributes(typeof(AnyThreadAttribute), false));
    }

    [Fact]
    public void WithoutTheServicesAllowSocketsKeepsTheKindsAndSaysWhyNothingElseWorks()
    {
        try
        {
            var setup = Gems.AllowSockets(GemSocketKinds.Rings | GemSocketKinds.Shields, 0.1);
            Assert.Equal(GemSocketKinds.Rings | GemSocketKinds.Shields, setup.Kinds);
            Assert.Equal(GemSocketKinds.Rings | GemSocketKinds.Shields, Gems.SocketKinds);
            Assert.False(setup.Server);
            Assert.False(string.IsNullOrEmpty(setup.ServerReason));
            Assert.False(setup.ForgeLists);
            Assert.False(string.IsNullOrEmpty(setup.ForgeReason));
        }
        finally { Gems.AllowSockets(GemSocketKinds.None); }
        Assert.Equal(GemSocketKinds.None, Gems.SocketKinds);
    }
}
