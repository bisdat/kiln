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
