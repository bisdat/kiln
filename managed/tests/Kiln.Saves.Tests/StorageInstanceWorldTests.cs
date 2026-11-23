// SPDX-License-Identifier: MIT
using Xunit;
namespace Kiln;

[Collection("instance data")]
public sealed class StorageInstanceWorldTests
{
    [Fact]
    public void Character_witness_includes_storage_and_restores_item_data_in_distinct_slots()
    {
        using var h = new InstanceHarness();
        var player = h.World.Join(InstanceHarness.Hero);
        var holder = h.World.Chest(Guid.NewGuid(), default);
        var fields = new InstanceFields(new ItemId(123), 1, 5, 0, 0, 0, 0, default, 0, 0, default, 0);
        var bagItem = h.World.Give(player, 0, fields);
        var storedFields = fields with { Level = 6 };
        var storedItem = h.World.Give(holder, 0, storedFields);
        var map = new StorageInstanceWorld(h.World, p => p.Holder == holder ? new(player, 1024 + p.FlatSlot) : p,
            p => p == player ? [player, holder] : [p], _ => true);
        InstanceHarness.Store.Instances.World = map;
        h.Tag(bagItem, 11);
        h.Tag(storedItem, 22);
        h.Save(1);
        var items = map.ReadHolder(player, out var initialized);
        Assert.True(initialized);
        Assert.Equal(new[] { 0, 1024 }, items.Select(i => i.Place.FlatSlot).ToArray());
        Assert.Equal(player, map.Read(storedItem)!.Value.Place.Holder);
        var world = new FakeInstanceWorld(9000);
        var newPlayer = world.Join(InstanceHarness.Hero);
        var newHolder = world.Chest(Guid.NewGuid(), default);
        var newBag = world.Give(newPlayer, 0, fields);
        var newStored = world.Give(newHolder, 0, storedFields);
        h.Restart(1, world, tick: false);
        InstanceHarness.Store.Instances.World = new StorageInstanceWorld(world,
            p => p.Holder == newHolder ? new(newPlayer, 1024 + p.FlatSlot) : p,
            p => p == newPlayer ? [newPlayer, newHolder] : [p], _ => true);
        h.Tick();
        Assert.Equal(11, h.Count(newBag));
        Assert.Equal(22, h.Count(newStored));
    }

    [Fact]
    public void Waiting_storage_defers_the_whole_character_witness()
    {
        var inner = new FakeInstanceWorld();
        var player = inner.Join(InstanceHarness.Hero);
        var map = new StorageInstanceWorld(inner, p => p, p => [p], _ => false);
        Assert.Empty(map.ReadHolder(player, out var ready));
        Assert.False(ready);
    }
}
