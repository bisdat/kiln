// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using Kiln;
using Xunit;

public sealed class UiItemDetailTests
{
    [Fact]
    public void Data_only_API_cannot_confuse_server_inventory_entities_with_client_entities()
    {
        Assert.DoesNotContain(typeof(UiDraw).GetMethods(), m => m.Name == "ItemDetail");
        Assert.DoesNotContain(typeof(Ui.Values).GetMethods(), m => m.Name == "ItemDetail" &&
            m.GetParameters().Any(p => p.ParameterType == typeof(InventorySlot) || p.ParameterType == typeof(ItemInfo)));
    }

    [Fact]
    public void Type_query_exposes_level_and_rarity_without_a_dummy_entity()
    {
        Assert.NotNull(typeof(Ui.Values).GetMethod("ItemDetail", [typeof(ItemId), typeof(int?), typeof(ItemRarity?)]));
    }

    [Fact]
    public void Invalid_queries_are_rejected_before_asking_for_a_capability()
    {
        Assert.Throws<ArgumentException>(() => Ui.Values.ItemDetail(ItemId.None));
        Assert.Throws<ArgumentOutOfRangeException>(() => Ui.Values.ItemDetail(new EntityId(1), level: -1));
        Assert.Throws<ArgumentOutOfRangeException>(() => Ui.Values.ItemDetail(new EntityId(1), rarity: (ItemRarity)255));
    }

    [Fact]
    public unsafe void Native_records_match_the_C_contract()
    {
        Assert.Equal(20, sizeof(UiItemDetailNative.Query));
        Assert.Equal(4112, sizeof(UiItemDetailNative.Line));
        Assert.Equal(56, sizeof(UiItemDetailNative.Snapshot));
        Assert.Equal(16, sizeof(UiItemDetailNative.Table));
        Assert.Equal(8, Marshal.OffsetOf<UiItemDetailNative.Table>(nameof(UiItemDetailNative.Table.Copy)).ToInt32());
    }
}
