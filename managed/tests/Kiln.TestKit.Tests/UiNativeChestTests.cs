// SPDX-License-Identifier: MIT
using Xunit;
namespace Kiln.TestKit.Tests;
[Collection("UiInventoryFake")]
public sealed class UiNativeChestTests
{
    [Fact]
    public void ButtonsBelongToTheNativeChestRowAndReceiveTheCurrentContainer()
    {
        using var h = new KilnHarness(); h.Step();
        var seen = new List<InventoryScreen>();
        using var button = h.Run(() => Ui.Inventory.AddChestButton("Count chest", seen.Add));
        Assert.Empty(h.UiChest.Buttons); Assert.False(h.UiChest.Press(0));
        h.UiInventory.OpenChest(new EntityId(4100));
        Assert.Equal(["Count chest"], h.UiChest.Buttons);
        Assert.True(h.UiChest.Press(0));
        Assert.Equal(new InventoryScreen(InventoryScreenKind.Chest, HudScreen.StorageChest, new EntityId(4100)), Assert.Single(seen));
        h.UiInventory.OpenChest(new EntityId(4200)); Assert.True(h.UiChest.Press(0));
        Assert.Equal(new EntityId(4200), seen[1].Container);
        h.UiInventory.OpenBackpack(); Assert.Empty(h.UiChest.Buttons); Assert.False(h.UiChest.Press(0));
        h.UiInventory.OpenChest(EntityId.None); Assert.Empty(h.UiChest.Buttons);
        h.UiInventory.OpenChest(new EntityId(4300)); button.Dispose(); button.Dispose();
        Assert.Empty(h.UiChest.Buttons); Assert.False(h.UiChest.Press(0));
    }
    [Fact]
    public void CaptionsAreUniqueAndTheRowHasFourPlaces()
    {
        using var h = new KilnHarness(); h.Step();
        using var a = h.Run(() => Ui.Inventory.AddChestButton("A", _ => { }));
        Assert.Throws<InvalidOperationException>(() => h.Run(() => Ui.Inventory.AddChestButton("A", _ => { })));
        using var b = h.Run(() => Ui.Inventory.AddChestButton("B", _ => { }));
        using var c = h.Run(() => Ui.Inventory.AddChestButton("C", _ => { }));
        using var d = h.Run(() => Ui.Inventory.AddChestButton("D", _ => { }));
        Assert.Throws<InvalidOperationException>(() => h.Run(() => Ui.Inventory.AddChestButton("E", _ => { })));
        a.Dispose();
        using var e = h.Run(() => Ui.Inventory.AddChestButton("E", _ => { }));
    }
    [Fact]
    public void ArgumentsAndCapabilitiesFailClosed()
    {
        using var h = new KilnHarness(); h.Step();
        Assert.Throws<ArgumentNullException>(() => h.Run(() => Ui.Inventory.AddChestButton(null!, _ => { })));
        Assert.Throws<ArgumentNullException>(() => h.Run(() => Ui.Inventory.AddChestButton("A", null!)));
        foreach (var bad in new[] { "", " ", "A\0B", new string('é', 65) })
            Assert.Throws<ArgumentException>(() => h.Run(() => Ui.Inventory.AddChestButton(bad, _ => { })));
        h.Capabilities.Override(Ui.Inventory.ChestButtonCapability, false);
        Assert.Throws<KilnUnavailableException>(() => h.Run(() => Ui.Inventory.AddChestButton("A", _ => { })));
    }
    [Fact]
    public void FaultedCallbacksAreRemovedAfterFiveConsecutiveFailures()
    {
        using var h = new KilnHarness(); h.Step(); var calls = 0;
        using var button = h.Run(() => Ui.Inventory.AddChestButton("Fail", _ => { calls++; throw new InvalidOperationException("button test"); }));
        h.UiInventory.OpenChest(new EntityId(4100));
        for (var i = 0; i < 7; i++) Assert.False(h.UiChest.Press(0));
        Assert.Equal(5, calls); Assert.Empty(h.UiChest.Buttons);
    }
    [Fact]
    public unsafe void TheServiceIsAnIndependentV1Table()
    {
        var t = (uint*)KitBackend.Instance.Service("kiln.ui.chest", 1);
        Assert.True(t != null); Assert.Equal((24u, 1u), (t[0], t[1]));
    }
}
