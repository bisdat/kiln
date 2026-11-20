// SPDX-License-Identifier: MIT
// The kit's fake kiln.ui.inventory (Fakes/UiInventory/): Ui.Inventory's own code on it. OnClick's subscriptions and the
// press rules (a drawn frame, a screen with a pointer, button and exact modifiers, take or watch), Hovered, the marks as
// Kiln.Ui decides them (MarkOf), Ui.Values.Slots on the drawn frame, the rows a test switches off.
using Xunit;

namespace Kiln.TestKit.Tests;

[Collection("UiInventoryFake")]
public sealed unsafe class UiInventoryFakeTests
{
    private static readonly FakeInventorySlot Sword = new(0xAA, 1, ItemEntity: 900, Holder: 77, InventoryPage: 0x5000, SlotIndex: 2);
    private static readonly FakeInventorySlot Wood = new(0xBB, 60, ItemEntity: 0, Holder: 77, InventoryPage: 0x5000, SlotIndex: 3);

    [Fact]
    public void TheTableIsTheHeadersV1()
    {
        // kiln_ui_inventory.h v1: the header and four functions
        var t = (uint*)KitBackend.Instance.Service("kiln.ui.inventory", 1);  // KilnServiceHeader { size, version }
        Assert.True(t != null);
        Assert.Equal((40u, 1u), (t[0], t[1]));
    }

    [Fact]
    public void AClickReachesTheMatchingHandlersWithTheItemsIdentity()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var taken = new List<SlotClick>();
        var watched = new List<SlotClick>();
        using var a = kiln.Run(() => Ui.Inventory.OnClick(new SlotClickOptions { Modifiers = SlotModifiers.Ctrl }, taken.Add));
        using var b = kiln.Run(() => Ui.Inventory.OnClick(new SlotClickOptions { Button = SlotButton.Right, Take = false }, watched.Add));
        Assert.Equal(2, kiln.UiInventory.ClickSubscriptions);

        Assert.Equal(new FakeSlotClickResult(0, false), kiln.UiInventory.Click(0, SlotButton.Left, SlotModifiers.Ctrl));  // nothing drawn
        kiln.UiInventory.Draw(HudScreen.StorageChest, Sword, Wood);
        Assert.Equal([Sword, Wood], kiln.UiInventory.Drawn);

        Assert.Equal(new FakeSlotClickResult(1, true), kiln.UiInventory.Click(0, SlotButton.Left, SlotModifiers.Ctrl));
        var c = Assert.Single(taken);
        Assert.Equal((new EntityId(900), new ItemId(0xAA), new EntityId(77), 2, new EntityId(0x5000)),
            (c.ItemEntity, c.Item, c.Holder, (int)c.Slot.SlotIndex, c.Slot.InventoryPage));
        Assert.Equal((SlotButton.Left, SlotModifiers.Ctrl, true, HudScreen.StorageChest), (c.Button, c.Modifiers, c.Taken, c.Screen.Hud));

        // modifiers are exact; a plain left click matches nobody and reaches the game
        Assert.Equal(new FakeSlotClickResult(0, false), kiln.UiInventory.Click(0, SlotButton.Left, SlotModifiers.Ctrl | SlotModifiers.Shift));
        Assert.Equal(new FakeSlotClickResult(0, false), kiln.UiInventory.Click(0));
        // a watcher sees the click, the game keeps it
        Assert.Equal(new FakeSlotClickResult(1, false), kiln.UiInventory.Click(1, SlotButton.Right));
        Assert.Equal((new EntityId(0), new ItemId(0xBB), 60u, false), (watched[0].ItemEntity, watched[0].Item, watched[0].Slot.Count, watched[0].Taken));
        Assert.Single(taken);
    }

    [Fact]
    public void NoClickCountsInThePlayViewOrAfterDispose()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var got = new List<SlotClick>();
        var reg = kiln.Run(() => Ui.Inventory.OnClick(new SlotClickOptions(), got.Add));
        kiln.UiInventory.Draw(HudScreen.InGame, Sword);  // the hotbar: a click attacks
        Assert.Equal(new FakeSlotClickResult(0, false), kiln.UiInventory.Click(0));
        kiln.UiInventory.Draw(HudScreen.Backpack, Sword);
        Assert.Equal(new FakeSlotClickResult(1, true), kiln.UiInventory.Click(0));
        reg.Dispose();
        Assert.Equal(0, kiln.UiInventory.ClickSubscriptions);
        Assert.Equal(new FakeSlotClickResult(0, false), kiln.UiInventory.Click(0));
        Assert.Single(got);
        Assert.Throws<ArgumentOutOfRangeException>(() => kiln.UiInventory.Click(5));
    }

    [Fact]
    public void ARowSwitchedOffIsKilnUnavailable()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        kiln.Capabilities.Override(Ui.Inventory.ClickCapability, false);
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => Ui.Inventory.OnClick(new SlotClickOptions(), _ => { })));
        kiln.Capabilities.ClearOverride(Ui.Inventory.ClickCapability);
        kiln.Capabilities.Override(Ui.Inventory.MarkCapability, false);
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => Ui.Inventory.Mark(new EntityId(900), new SlotMark(Color.Red))));
        Assert.Throws<KilnUnavailableException>(() => { kiln.Run<VisibleSlotsResult>(() => Ui.Values.Slots()); });
        kiln.Capabilities.ClearOverride(Ui.Inventory.MarkCapability);
        Assert.Equal(0, kiln.UiInventory.ClickSubscriptions);
    }

    [Fact]
    public void HoveredMarksAndTheSlotsReadFollowTheDrawnFrame()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        Assert.Null(kiln.Run<VisibleSlot?>(() => Ui.Inventory.Hovered));
        Assert.False(kiln.Run<VisibleSlotsResult>(() => Ui.Values.Slots()).Ok);  // no frame drawn yet
        kiln.UiInventory.Draw(HudScreen.Backpack, Sword, Wood);
        kiln.UiInventory.Hover(1);
        Assert.Equal(new ItemId(0xBB), kiln.Run<VisibleSlot?>(() => Ui.Inventory.Hovered)!.Item);
        var read = kiln.Run<VisibleSlotsResult>(() => Ui.Values.Slots());
        Assert.True(read.Ok);
        Assert.Equal([new EntityId(900), new EntityId(0)], read.Snapshot!.Slots.Select(s => s.ClientEntity));
        Assert.Equal(HudScreen.Backpack, read.Snapshot.Screen.Hud);

        var red = new SlotMark(Color.Red);
        var tint = new SlotMark(Color.Yellow.WithAlpha(80), SlotMarkStyle.Fill);
        try
        {
            Assert.Null(kiln.UiInventory.MarkOf(0));
            kiln.Run(() => Ui.Inventory.Mark(new EntityId(900), red));
            using var rule = kiln.Run(() => Ui.Inventory.MarkWhen(s => s.Count >= 50 ? tint : null));
            Assert.Equal(red, kiln.UiInventory.MarkOf(0));
            Assert.Equal(tint, kiln.UiInventory.MarkOf(1));
            kiln.UiInventory.Draw(HudScreen.StorageChest, Wood, Sword);  // the sword moved: its mark follows the item
            Assert.Equal(red, kiln.UiInventory.MarkOf(1));
            kiln.Run(() => Ui.Inventory.Unmark(new EntityId(900)));
            Assert.Null(kiln.UiInventory.MarkOf(1));
        }
        finally
        {
            kiln.Run(Ui.Inventory.ClearMarks);
        }
    }
}
