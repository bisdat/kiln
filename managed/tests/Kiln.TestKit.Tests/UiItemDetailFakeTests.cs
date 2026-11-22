// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.TestKit;
using Xunit;

public sealed class UiItemDetailFakeTests
{
    [Fact]
    public void Captured_panels_are_copies_and_do_not_cross_instance_or_variant()
    {
        using var h = new KilnHarness();
        var item = new ItemId(0xAA); var entity = new EntityId(900);
        Assert.False(Ui.Values.ItemDetail(entity).Ok);
        h.UiItemDetail.Capture(item, entity, 4, ItemRarity.Rare,
            new UiItemDetailLine("Damage", 42.5f), new UiItemDetailLine("42.5", null));
        var first = Ui.Values.ItemDetail(entity);
        Assert.True(first.Ok);
        Assert.Equal(42.5f, first.Snapshot!.Lines[0].Value);
        Assert.False(Ui.Values.ItemDetail(new EntityId(901)).Ok);
        Assert.False(Ui.Values.ItemDetail(entity, item, 5, ItemRarity.Rare).Ok);
        Assert.True(Ui.Values.ItemDetail(item).Ok);
        h.UiItemDetail.Capture(item, entity, 4, ItemRarity.Rare, new UiItemDetailLine("new", 99));
        Assert.Equal("Damage", first.Snapshot.Lines[0].Text);
        Assert.Equal("new", Ui.Values.ItemDetail(entity).Snapshot!.Lines[0].Text);
        h.UiItemDetail.Clear();
        Assert.False(Ui.Values.ItemDetail(entity).Ok);
    }
}
