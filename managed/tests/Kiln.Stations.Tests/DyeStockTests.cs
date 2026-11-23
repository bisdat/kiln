// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.TestKit;
using Xunit;
namespace Kiln.StationsTests;
public sealed class DyeStockTests
{
    [Fact]
    public void The_same_filtered_player_sources_supply_only_color_palettes_without_a_base()
    {
        using var h = new KilnHarness();
        var player = h.Players.Join("Dyer", rights: PlayerPermissions.AccessInventories);
        var other = h.Players.Join("Other", rights: PlayerPermissions.AccessInventories);
        h.Step();
        var dye = h.Items.AddItem("blue", 100, ItemCategory.ColorPalette);
        var denied = h.Items.AddItem("red", 100, ItemCategory.ColorPalette);
        var wood = h.Items.AddItem("wood", 100);
        var chest = h.Entities.Create();
        var inv = h.Items.AddContainer(chest); inv.Put(dye, 3); inv.Put(denied, 7); inv.Put(wood, 9);
        using var source = h.Run(() => Crafting.AddSource(player.Entity, chest, new CraftSourceOptions { Items = [denied] }));
        var rows = h.Run(() => DyeStock.Build(player.Entity, supported: true));
        Assert.True(rows.Supported);
        Assert.Equal(new CraftStockRow(dye.Value, 3), Assert.Single(rows.Rows));
        Assert.Equal(3, h.Crafting.Dyes(player.Entity)[dye]);
        Assert.Empty(h.Crafting.Dyes(other.Entity));
        Assert.Empty(h.Run(() => DyeStock.Build(other.Entity, supported: true)).Rows);
        source.Dispose();
        Assert.Empty(h.Run(() => DyeStock.Build(player.Entity, supported: true)).Rows);
    }

    [Fact]
    public void Client_offers_require_a_live_supported_Kiln_server_and_an_open_tint_screen()
    {
        var rows = new DyeStockRows(true, [new CraftStockRow(123, 4)]);
        Assert.Empty(DyeStock.Visible(rows, kilnServer: false, open: true).Rows);
        Assert.Empty(DyeStock.Visible(rows with { Supported = false }, kilnServer: true, open: true).Rows);
        Assert.Empty(DyeStock.Visible(rows, kilnServer: true, open: false).Rows);
        Assert.Equal(rows.Rows, DyeStock.Visible(rows, kilnServer: true, open: true).Rows);
    }

    [Fact]
    public void Unsupported_server_never_publishes_registered_dyes()
    {
        using var h = new KilnHarness();
        var p = h.Players.Join("Dyer", rights: PlayerPermissions.AccessInventories); h.Step();
        var dye = h.Items.AddItem("blue", 100, ItemCategory.ColorPalette);
        var c = h.Entities.Create(); h.Items.AddContainer(c).Put(dye, 3);
        using var source = h.Run(() => Crafting.AddSource(p.Entity, c));
        var rows = h.Run(() => DyeStock.Build(p.Entity, supported: false));
        Assert.False(rows.Supported); Assert.Empty(rows.Rows);
    }

    [Fact]
    public void The_snapshot_carries_item_counts_and_explicit_support_not_chest_slots()
    {
        var message = new DyeStockRows(true, [new CraftStockRow(123, 7)]);
        var writer = new NetWriter(); DyeStockRows.Encode(message, writer);
        var reader = new NetReader(writer.ToArray());
        var decoded = DyeStockRows.Decode(ref reader);
        Assert.True(decoded.Supported); Assert.Equal(message.Rows, decoded.Rows);
        Assert.Equal(11, writer.ToArray().Length);
    }
}
