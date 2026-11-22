// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.TestKit;
using Xunit;
namespace Kiln.StationsTests;

public sealed class CraftingSourcesTests
{
    [Fact]
    public void Registrations_copy_the_filter_and_dispose_removes_them()
    {
        using var h = new KilnHarness();
        var player = h.Players.Join("Crafter", rights: PlayerPermissions.AccessInventories);
        h.Step();
        var item = h.Items.AddItem("wood", 100);
        var other = h.Items.AddItem("stone", 100);
        var chest = h.Entities.Create(); var inv = h.Items.AddContainer(chest); inv.Put(item, 7); inv.Put(other, 9);
        ItemId[] filter = [item];
        using var source = h.Run(() => Crafting.AddSource(player.Entity,chest,new CraftSourceOptions {Filter=CraftSourceFilter.Allow,Items=filter}));
        filter[0] = other;
        Assert.Single(h.Crafting.Sources);
        Assert.Equal(7,h.Crafting.Stock(player.Entity).GetValueOrDefault(item));
        Assert.False(h.Crafting.Stock(player.Entity).ContainsKey(other));
        source.Dispose(); source.Dispose();
        Assert.Empty(h.Crafting.Sources);
        Assert.Empty(h.Crafting.Stock(player.Entity));
    }

    [Fact]
    public void Access_and_player_identity_are_checked_when_counts_are_read()
    {
        using var h = new KilnHarness();
        var player = h.Players.Join("Owner", rights: PlayerPermissions.AccessInventories);
        var other = h.Players.Join("Other", rights: PlayerPermissions.AccessInventories);
        var denied = h.Players.Join("Visitor");
        h.Step();
        var item = h.Items.AddItem("wood",100);
        var chest = h.Entities.Create(); h.Items.AddContainer(chest).Put(item,4);
        using var one = h.Run(() => Crafting.AddSource(player.Entity,chest));
        using var two = h.Run(() => Crafting.AddSource(denied.Entity,chest));
        Assert.Equal(4,h.Crafting.Stock(player.Entity)[item]);
        Assert.Empty(h.Crafting.Stock(other.Entity));
        Assert.Empty(h.Crafting.Stock(denied.Entity));
        h.Run(() => Assert.Throws<ArgumentException>(() => Crafting.AddSource(player.Entity,other.Entity)));
        h.Run(() => Assert.Throws<ArgumentException>(() => Crafting.AddSource(player.Entity,h.Entities.Create())));
    }

    [Fact]
    public void Deny_filter_and_duplicate_containers_form_a_per_item_union()
    {
        using var h = new KilnHarness();
        var player = h.Players.Join("Crafter", rights: PlayerPermissions.AccessInventories);
        h.Step();
        var wood = h.Items.AddItem("wood",100); var stone = h.Items.AddItem("stone",100);
        var chest = h.Entities.Create(); var inv=h.Items.AddContainer(chest);inv.Put(wood,3);inv.Put(stone,5);
        using var a = h.Run(() => Crafting.AddSource(player.Entity,chest,new CraftSourceOptions {Items=[stone]}));
        using var b = h.Run(() => Crafting.AddSource(player.Entity,chest,new CraftSourceOptions {Order=CraftSourceOrder.AfterStationOutputs,Filter=CraftSourceFilter.Allow,Items=[wood]}));
        Assert.Equal(3,h.Crafting.Stock(player.Entity)[wood]);
        Assert.False(h.Crafting.Stock(player.Entity).ContainsKey(stone));
    }

    [Fact]
    public void Linked_stock_reaches_the_crafting_snapshot_without_a_base()
    {
        using var h = new KilnHarness();
        CraftStock.ResetForTests(); StationsCore.UseBackend(null);
        try
        {
            var player = h.Players.Join("Crafter", rights: PlayerPermissions.AccessInventories);
            h.Step();
        var wood = h.Items.AddItem("wood", 100);
            var chest = h.Entities.Create(); h.Items.AddContainer(chest).Put(wood, 4);
            player.Inventory.Put(wood, 2);
            using var source = h.Run(() => Crafting.AddSource(player.Entity, chest));
            var rows = h.Run(() => CraftStock.Build(player.Entity));
            Assert.NotNull(rows);
            Assert.Equal(new CraftStockRow(wood.Value, 6), Assert.Single(rows.Rows));
            source.Dispose();
            Assert.Null(h.Run(() => CraftStock.Build(player.Entity)));
        }
        finally { CraftStock.ResetForTests(); StationsCore.UseBackend(null); }
    }

    [Fact]
    public void A_reused_player_slot_cannot_inherit_the_previous_players_source()
    {
        using var h = new KilnHarness();
        var player = h.Players.Join("First", rights: PlayerPermissions.AccessInventories); h.Step();
        var item = h.Items.AddItem("wood", 100);
        var chest = h.Entities.Create(); h.Items.AddContainer(chest).Put(item, 3);
        using var source = h.Run(() => Crafting.AddSource(player.Entity, chest));
        player.Leave(); h.Step();
        var next = h.Players.Join("Next", rights: PlayerPermissions.AccessInventories); h.Step();
        Assert.Equal(player.Entity, next.Entity);
        Assert.Empty(h.Crafting.Stock(next.Entity));
    }

    private sealed class SourceMod : Mod
    {
        public override void OnLoad() => Crafting.AddSource(new EntityId(1), Items.FindInventories(InventoryKinds.Containers).Single());
    }

    [Fact]
    public void Unloading_the_registering_mod_removes_its_sources()
    {
        using var h = new KilnHarness();
        h.Players.Join("Crafter", rights: PlayerPermissions.AccessInventories);
        var chest=h.Entities.Create();h.Items.AddContainer(chest);
        h.Step();
        var mod=h.Load<SourceMod>(new ModManifest("source-test"));
        h.Step(2);
        Assert.True(mod.IsLoaded,mod.Error); Assert.Single(h.Crafting.Sources);
        h.Unload(mod); Assert.Empty(h.Crafting.Sources);
    }

    [Fact]
    public void Invalid_arguments_wrong_thread_and_disabled_capability_are_refused()
    {
        using var h=new KilnHarness();var chest=h.Entities.Create();h.Items.AddContainer(chest);
        Assert.Throws<InvalidOperationException>(() => Crafting.AddSource(new EntityId(1),chest));
        h.Run(() => Assert.Throws<ArgumentException>(() => Crafting.AddSource(default,chest)));
        h.Run(() => Assert.Throws<ArgumentOutOfRangeException>(() => Crafting.AddSource(new EntityId(1),chest,new CraftSourceOptions {Order=(CraftSourceOrder)9})));
        h.Capabilities.Override(Crafting.SourcesCapability,false,"fixture off");
        var e=h.Run(() => Assert.Throws<KilnUnavailableException>(() => Crafting.AddSource(new EntityId(1),chest)));
        Assert.Equal(Crafting.SourcesCapability,e.Capability);
    }
}
