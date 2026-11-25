// SPDX-License-Identifier: MIT
// GitHub #132 (0.2.0-dev.31.12): CraftSourceOptions.Fills and .Building on the real managed API against the kit's
// kiln.stations fake: the flags, their counts (source_stock FILLS / BUILDING), the trackers behind the station screen's
// press and the building snapshot. The fills and the placements themselves are native (native/services/items/tests:
// craft_sources_test, build_sources_test, craft_payment_test).
using Kiln;
using Kiln.TestKit;
using Xunit;
namespace Kiln.StationsTests;

public sealed class LinkPurposeTests : IDisposable
{
    public void Dispose() => BuildStock.ResetForTests();

    [Fact]
    public void Fills_and_Building_are_off_by_default_and_recorded_when_asked_for()
    {
        Assert.False(new CraftSourceOptions().Fills);
        Assert.False(new CraftSourceOptions().Building);
        using var h = new KilnHarness();
        var player = h.Players.Join("Builder", rights: PlayerPermissions.AccessInventories);
        h.Step();
        var stone = h.Items.AddItem("stone", 100);
        var a = h.Entities.Create(); h.Items.AddContainer(a).Put(stone, 2);
        var b = h.Entities.Create(); h.Items.AddContainer(b).Put(stone, 3);
        var c = h.Entities.Create(); h.Items.AddContainer(c).Put(stone, 5);
        using var crafting = h.Run(() => Crafting.AddSource(player.Entity, a));
        using var fills = h.Run(() => Crafting.AddSource(player.Entity, b, new CraftSourceOptions { Fills = true }));
        using var building = h.Run(() => Crafting.AddSource(player.Entity, c, new CraftSourceOptions { Building = true, Locks = true }));
        Assert.Equal([(false, false, false), (true, false, false), (false, true, true)],
            h.Crafting.Sources.Select(s => (s.Fills, s.Building, s.Locks)));
        // crafting counts every link; a fill and a placement only their opted-in ones
        Assert.Equal(10, h.Crafting.Stock(player.Entity)[stone]);
        Assert.Equal(3, h.Crafting.FillStock(player.Entity)[stone]);
        Assert.Equal(5, h.Crafting.BuildStock(player.Entity)[stone]);
        Assert.Equal(new StationsNative.Stack { Item = stone.Value, Count = 5 }, Assert.Single(h.Run(() => CraftingCore.BuildStockRows(player.Entity)).Rows));
        Assert.Equal(new StationsNative.Stack { Item = stone.Value, Count = 3 }, Assert.Single(h.Run(() => CraftingCore.FillStock(player.Entity)).Rows));
        // the trackers: the press follows a fill link, the snapshot a building link
        Assert.True(LinkPurposes.Fills.Has(player.Entity));
        Assert.Equal((true, 1), BuildStock.SessionOf(player.Entity));
        fills.Dispose(); building.Dispose();
        Assert.False(LinkPurposes.Fills.Has(player.Entity));
        Assert.Equal((true, 0), BuildStock.SessionOf(player.Entity));  // closes at the next tick, after "unsupported" goes out
        Assert.Empty(h.Crafting.FillStock(player.Entity));
        Assert.Empty(h.Crafting.BuildStock(player.Entity));
        Assert.Equal(2, h.Crafting.Stock(player.Entity)[stone]);
    }

    [Fact]
    public void A_purpose_uses_the_links_filter_and_the_players_current_permission()
    {
        using var h = new KilnHarness();
        var player = h.Players.Join("Builder", rights: PlayerPermissions.AccessInventories);
        var visitor = h.Players.Join("Visitor");
        h.Step();
        var stone = h.Items.AddItem("stone", 100);
        var wood = h.Items.AddItem("wood", 100);
        var chest = h.Entities.Create(); var inv = h.Items.AddContainer(chest); inv.Put(stone, 4); inv.Put(wood, 6);
        using var allow = h.Run(() => Crafting.AddSource(player.Entity, chest,
            new CraftSourceOptions { Building = true, Fills = true, Filter = CraftSourceFilter.Allow, Items = [wood] }));
        using var refused = h.Run(() => Crafting.AddSource(visitor.Entity, chest, new CraftSourceOptions { Building = true }));
        Assert.Equal(6, Assert.Single(h.Crafting.BuildStock(player.Entity)).Value);
        Assert.Equal(6, Assert.Single(h.Crafting.FillStock(player.Entity)).Value);
        Assert.Empty(h.Crafting.BuildStock(visitor.Entity));  // no AccessInventories
    }

    [Fact]
    public void The_purpose_flags_reach_kiln_stations()
    {
        Assert.Equal((2u, 4u, 4u, 8u, 0x100u), (StationsNative.SourceFills, StationsNative.SourceBuilding, StationsNative.StockFills,
            StationsNative.StockBuilding, StationsNative.FillLinked));
        Assert.False(CraftingCore.HasPurposeMembers((uint)StationsNative.TableLocks));
        Assert.True(CraftingCore.HasPurposeMembers((uint)StationsNative.TableBuild));
        Assert.Equal("crafting.build_sources", Crafting.BuildSourcesCapability);
    }
}
