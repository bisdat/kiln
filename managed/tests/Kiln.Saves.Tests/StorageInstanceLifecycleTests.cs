// SPDX-License-Identifier: MIT
using Xunit;
namespace Kiln;

[Collection("instance data")]
public sealed class StorageInstanceLifecycleTests
{
    private static readonly InstanceFields Fields = new(new ItemId(123), 1, 6, 0, 0, 0, 0, default, 0, 0, default, 0);

    private sealed class Visit : IDisposable
    {
        internal readonly InstanceHarness H = new();
        internal readonly EntityId Player;
        internal EntityId Holder;
        internal bool Visible = true;
        internal ItemInstanceRef Item;
        internal Visit()
        {
            Player = H.World.Join(InstanceHarness.Hero);
            Holder = H.World.Chest(Guid.NewGuid(), default);
            Item = H.World.Give(Holder, 3, Fields);
            InstanceHarness.Store.Instances.World = new StorageInstanceWorld(H.World,
                p => p.Holder == Holder ? new(Player, 1024 + p.FlatSlot) : p,
                p => p == Player && Visible ? [Player, Holder] : [p], _ => true);
            H.Tag(Item, 77); // New metadata: deliberately no world save before parking.
        }
        internal void Park()
        {
            InstanceHarness.Store.Instances.StorageParked(InstanceHarness.Hero, Player, Holder, 1024, "mod:bag");
            Visible = false;
            H.World.Destroy(Item);
            H.World.Holders.Remove(Holder.Value);
        }
        public void Dispose() => H.Dispose();
    }

    [Fact]
    public void Unregister_before_first_save_preserves_values_in_character_generation()
    {
        using var v = new Visit();
        v.Park();
        Assert.Empty(v.H.Events); // No mod events from the synchronous native housekeeping callback.
        v.H.World.Step += InstanceStore.LeaveWindowSteps + 1;
        v.H.Tick();
        v.H.Save(1);
        Assert.Empty(v.H.Of<InstanceDataLostEvent>());
        Assert.Single(v.H.Parked());
        Assert.NotNull(v.H.Bundle(1, InstanceStore.CharacterPrefix + InstanceHarness.Hero.Text + "/sword-mod"));
    }

    [Fact]
    public void Rebuild_reattaches_unsaved_metadata_synchronously_but_defers_user_events()
    {
        using var v = new Visit();
        v.Park();
        v.H.Tick();
        v.H.Events.Clear();
        v.Holder = v.H.World.Chest(Guid.NewGuid(), default);
        var rebuilt = v.H.World.Give(v.Holder, 3, Fields);
        v.Visible = true;
        InstanceHarness.Store.Instances.StorageRestored(InstanceHarness.Hero, v.Player);
        Assert.Empty(v.H.Events);
        Assert.Equal(77, v.H.Count(rebuilt));
        v.H.Tick();
        Assert.Single(v.H.Of<InstanceDataRestoredEvent>());
        Assert.Empty(v.H.Parked());
    }

    [Fact]
    public void Leave_between_park_and_save_retains_values_for_next_character_visit()
    {
        using var v = new Visit();
        v.Park();
        v.H.World.Leave(v.Player);
        v.H.Tick();
        v.H.Save(1);
        var world = new FakeInstanceWorld(9000);
        var player = world.Join(InstanceHarness.Hero);
        var holder = world.Chest(Guid.NewGuid(), default);
        var rebuilt = world.Give(holder, 3, Fields);
        v.H.Restart(1, world, tick: false);
        InstanceHarness.Store.Instances.World = new StorageInstanceWorld(world,
            p => p.Holder == holder ? new(player, 1024 + p.FlatSlot) : p,
            p => p == player ? [player, holder] : [p], _ => true);
        InstanceHarness.Store.Instances.StorageRestored(InstanceHarness.Hero, player);
        Assert.Equal(77, v.H.Count(rebuilt));
        Assert.Empty(v.H.Of<InstanceDataLostEvent>());
    }
    [Fact]
    public void Housekeeping_events_wait_for_regular_tick_even_if_a_world_stage_runs_first()
    {
        using var v = new Visit();
        v.Park();
        v.H.Tick();
        v.H.Events.Clear();
        v.Holder = v.H.World.Chest(Guid.NewGuid(), default);
        v.H.World.Give(v.Holder, 3, Fields);
        v.Visible = true;
        InstanceHarness.Store.Instances.StorageRestored(InstanceHarness.Hero, v.Player);
        v.H.StageOnly(1);
        Assert.Empty(v.H.Events);
        v.H.Tick();
        Assert.Single(v.H.Of<InstanceDataRestoredEvent>());
    }

    [Fact]
    public void Item_already_moved_out_without_a_tracker_notice_is_not_parked_with_the_old_holder()
    {
        using var v = new Visit();
        v.H.World.Move(v.Item, v.Player, 1, seen: false);
        InstanceHarness.Store.Instances.StorageParked(InstanceHarness.Hero, v.Player, v.Holder, 1024, "mod:bag");
        Assert.Equal(77, v.H.Count(v.Item));
        Assert.Empty(v.H.Parked());
    }

}
