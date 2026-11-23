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

    private static void ParkedDocument(params int[] items)
    {
        // The server copy as Kiln.Saves keeps it: the key parked (its mod is off) with its records, and the key's band.
        var records = new System.Text.Json.Nodes.JsonArray(items.Select((item, slot) => (System.Text.Json.Nodes.JsonNode)new System.Text.Json.Nodes.JsonObject
        {
            ["s"] = slot, ["b64"] = Convert.ToBase64String([(byte)slot, (byte)item, (byte)(item >> 8), 0, 0, 1, 0, 0]), ["item"] = item, ["n"] = 1,
        }).ToArray());
        var doc = PlayerStorageDocument.Empty(InstanceHarness.Hero.Id);
        doc.Data["revision"] = 2;
        doc.Data["current"]!["witness"] = "xxh3:00000000000000aa";
        doc.Data["current"]!["parked"] = new System.Text.Json.Nodes.JsonObject
        {
            ["mod:bag"] = new System.Text.Json.Nodes.JsonObject { ["slots"] = 8, ["records"] = records, ["nativeParkReason"] = 1 },
        };
        var bundle = InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + InstanceHarness.Hero.Text, PlayerStorageStore.Mod);
        bundle.Keys[PlayerStorageStore.Key] = new WorldStore.Slot { Json = doc.Write(32, PlayerStorageDocument.ServerMaxBytes) };
        bundle.Keys["instanceBands"] = new WorldStore.Slot { Json = "{\"mod:bag\":0}" };
    }

    [Fact]
    public void Parked_storage_keeps_item_metadata_while_its_records_stay_parked_across_sessions()
    {
        // The storage's mod stays off for more world loads than InstanceStore's three missed sessions: the native
        // records stay parked in the storage document, so their InstanceData must stay too.
        using var v = new Visit();
        v.Park();
        ParkedDocument(123);
        v.H.Tick();
        v.H.Save(1);
        for (uint session = 2; session <= 2 + InstanceStore.MaxMissedSessions; session++)
        {
            var world = new FakeInstanceWorld(9000 + 100 * session);
            world.Join(InstanceHarness.Hero);
            v.H.Restart(session - 1, world);
            v.H.Tick();
            v.H.Save(session);
        }
        Assert.Empty(v.H.Of<InstanceDataLostEvent>());
        Assert.Single(v.H.Parked());
        // The mod comes back: its storage is rebuilt and the item gets its metadata back.
        var last = new FakeInstanceWorld(20000);
        var player = last.Join(InstanceHarness.Hero);
        var holder = last.Chest(Guid.NewGuid(), default);
        var rebuilt = last.Give(holder, 3, Fields);
        v.H.Restart(3 + InstanceStore.MaxMissedSessions, last, tick: false);
        InstanceHarness.Store.Instances.World = new StorageInstanceWorld(last,
            p => p.Holder == holder ? new(player, 1024 + p.FlatSlot) : p,
            p => p == player ? [player, holder] : [p], _ => true);
        InstanceHarness.Store.Instances.StorageRestored(InstanceHarness.Hero, player);
        Assert.Equal(77, v.H.Count(rebuilt));
    }

    [Fact]
    public void Storage_band_metadata_without_parked_records_still_ages_out()
    {
        // Nothing parked holds the item any more (its storage was emptied elsewhere): the ordinary three-session rule.
        using var v = new Visit();
        v.Park();
        ParkedDocument(456);
        v.H.Tick();
        v.H.Save(1);
        for (uint session = 2; session <= 2 + InstanceStore.MaxMissedSessions; session++)
        {
            var world = new FakeInstanceWorld(9000 + 100 * session);
            world.Join(InstanceHarness.Hero);
            v.H.Restart(session - 1, world);
            v.H.Tick();
            v.H.Save(session);
        }
        Assert.NotEmpty(v.H.Of<InstanceDataLostEvent>());
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
