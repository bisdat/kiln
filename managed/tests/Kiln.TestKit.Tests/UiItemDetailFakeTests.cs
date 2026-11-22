// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.TestKit;
using Xunit;

public sealed class UiItemDetailFakeTests
{
    [Fact]
    public void Invalid_unicode_is_refused_instead_of_silently_replaced()
    {
        using var h = new KilnHarness();
        Assert.ThrowsAny<ArgumentException>(() => h.UiItemDetail.Capture(new ItemId(1), default, 1, null,
            new UiItemDetailLine("\ud800", null)));
    }

    [Fact]
    public async Task Type_queries_constrain_variants_and_reads_work_off_the_tick_thread()
    {
        using var h = new KilnHarness();
        var item = new ItemId(0xAA);
        h.UiItemDetail.Capture(item, new EntityId(1), 4, ItemRarity.Rare, new UiItemDetailLine("old", 42));
        h.UiItemDetail.Capture(item, new EntityId(2), 5, ItemRarity.Epic, new UiItemDetailLine("new", 77));
        Assert.Equal("new", Ui.Values.ItemDetail(item).Snapshot!.Lines[0].Text);
        Assert.Equal("old", (await Task.Run(() => Ui.Values.ItemDetail(item, 4, ItemRarity.Rare))).Snapshot!.Lines[0].Text);
        Assert.False(Ui.Values.ItemDetail(item, 4, ItemRarity.Epic).Ok);
    }

    [Fact]
    public void Cache_is_bounded_in_capture_order_not_read_order()
    {
        using var h = new KilnHarness();
        for (uint id = 1; id <= 64; ++id)
            h.UiItemDetail.Capture(new ItemId(1), new EntityId(id), 1, null, new UiItemDetailLine($"{id}", null));
        Assert.True(Ui.Values.ItemDetail(new EntityId(1)).Ok);
        h.UiItemDetail.Capture(new ItemId(1), new EntityId(65), 1, null, new UiItemDetailLine("65", null));
        Assert.False(Ui.Values.ItemDetail(new EntityId(1)).Ok);
        Assert.True(Ui.Values.ItemDetail(new EntityId(65)).Ok);
        h.UiItemDetail.Clear();
        Assert.False(Ui.Values.ItemDetail(new ItemId(1)).Ok);
    }

    [Fact]
    public void Recording_budgets_are_UTF8_bytes_and_reject_incomplete_panels()
    {
        using var h = new KilnHarness();
        var item = new ItemId(1);
        var maxRow = new UiItemDetailLine(new string('Ü', 2048), null);
        h.UiItemDetail.Capture(item, default, 1, null, Enumerable.Repeat(maxRow, 8).ToArray());
        Assert.Equal(8, Ui.Values.ItemDetail(item).Snapshot!.Lines.Count);
        Assert.Throws<ArgumentException>(() => h.UiItemDetail.Capture(item, default, 1, null,
            Enumerable.Repeat(maxRow, 9).ToArray()));
        Assert.Throws<ArgumentException>(() => h.UiItemDetail.Capture(item, default, 1, null,
            new UiItemDetailLine(new string('Ü', 2049), null)));
        Assert.Throws<ArgumentException>(() => h.UiItemDetail.Capture(item, default, 1, null,
            Enumerable.Repeat(new UiItemDetailLine("x", null), 65).ToArray()));
        Assert.Throws<ArgumentException>(() => h.UiItemDetail.Capture(item, default, 1, null,
            new UiItemDetailLine("x", float.NaN)));
    }

    [Fact]
    public void Disabled_capture_is_an_unavailable_capability_not_a_cache_miss()
    {
        using var h = new KilnHarness();
        h.Capabilities.Override(Ui.Values.ItemDetailCapability, false, "fixture: missing capture seam");
        Assert.Throws<KilnUnavailableException>(() => Ui.Values.ItemDetail(new ItemId(1)));
    }

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
