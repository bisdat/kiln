// SPDX-License-Identifier: MIT
using System.Numerics;
using Xunit;

namespace Kiln.TestKit.Tests;

/// <summary>The kit's kiln.client.crosshair fake (Fakes/Crosshair): Kiln.Client's Crosshair runs on it unchanged, and it
/// answers as the native service does (kiln_client_crosshair.h): no sample until the test aims, a fresh sample per aim
/// with the next sequence and the harness's step, stale past Crosshair.FreshFor, the row off throws on Read.</summary>
public sealed class CrosshairFakeTests
{
    [Fact]
    public void Nothing_is_aimed_at_until_the_test_aims()
    {
        using var h = new KilnHarness();
        h.Step();
        Assert.True(h.Run(() => Crosshair.Available));
        var none = h.Run(() => (Crosshair.Read(out var why), why));
        Assert.Null(none.Item1);
        Assert.Equal(CrosshairReason.NoSample, none.why);
        Assert.False(h.Run(() => Crosshair.TryGet(out _)));
        Assert.Equal(2, h.Crosshair.Reads);
    }

    [Fact]
    public void An_aim_at_an_entity_is_a_fresh_hit_with_its_fields()
    {
        using var h = new KilnHarness();
        h.Step();
        h.Crosshair.Aim(new WorldPosition(10, 20, 30), Vector3.UnitY, distance: 4.5f, entity: new EntityId(4711));
        var hit = h.Run(() => Crosshair.TryGet(out var x) ? x : default);
        Assert.True(hit.IsHit && hit.IsFresh && hit.HasEntity);
        Assert.Equal(new WorldPosition(10, 20, 30), hit.Position);
        Assert.Equal(Vector3.UnitY, hit.Normal);
        Assert.Equal(4.5f, hit.Distance);
        Assert.Equal(new EntityId(4711), hit.Entity);
        Assert.Null(hit.VoxelMaterial);
        Assert.Equal(0u, hit.GameStatus);
        Assert.Equal(1ul, hit.Sequence);
        Assert.Equal(h.CurrentStep, hit.ClientStep);
        Assert.Equal(FakeCrosshair.Session, hit.ClientSession);
    }

    [Fact]
    public void Terrain_has_a_material_and_no_entity_and_a_miss_hits_nothing()
    {
        using var h = new KilnHarness();
        h.Step();
        h.Crosshair.Aim(new WorldPosition(1, 2, 3), voxelMaterial: 37);
        var terrain = h.Run(() => Crosshair.Read(out _))!.Value;
        Assert.Equal((byte)37, terrain.VoxelMaterial);
        Assert.False(terrain.HasEntity);
        Assert.Null(terrain.Normal);
        Assert.Null(terrain.Distance);  // not given: not set
        h.Crosshair.AimAtNothing();
        var miss = h.Run(() => Crosshair.Read(out _))!.Value;
        Assert.False(miss.IsHit);
        Assert.Equal(31u, miss.GameStatus);
        Assert.Null(miss.Position);
        Assert.Equal(2ul, miss.Sequence);
    }

    [Fact]
    public void An_old_sample_is_stale_and_cleared_is_no_sample()
    {
        using var h = new KilnHarness();
        h.Step();
        h.Crosshair.Aim(new WorldPosition(0, 0, 0));
        h.Crosshair.Age(TimeSpan.FromSeconds(1));
        var stale = h.Run(() => (Crosshair.Read(out var why), why));
        Assert.Equal(CrosshairReason.Stale, stale.why);
        Assert.False(stale.Item1!.Value.IsFresh);
        Assert.False(h.Run(() => Crosshair.TryGet(out _)));
        h.Crosshair.Clear();
        Assert.Null(h.Run(() => Crosshair.Read(out _)));
    }

    [Fact]
    public void A_fault_reads_faulted()
    {
        using var h = new KilnHarness();
        h.Step();
        h.Crosshair.Aim(new WorldPosition(0, 0, 0));
        h.Crosshair.Fault();
        var r = h.Run(() => (Crosshair.Read(out var why), why));
        Assert.Null(r.Item1);
        Assert.Equal(CrosshairReason.Faulted, r.why);
    }

    [Fact]
    public void The_row_off_throws_on_read_and_try_get_is_false()
    {
        using var h = new KilnHarness();
        h.Step();
        h.Crosshair.Aim(new WorldPosition(0, 0, 0));
        h.Capabilities.Override(Crosshair.Capability, false);
        Assert.False(h.Run(() => Crosshair.Available));
        var e = Assert.Throws<KilnUnavailableException>(() => h.Run(() => Crosshair.Read(out _)));
        Assert.Equal(Crosshair.Capability, e.Capability);
        Assert.False(h.Run(() => Crosshair.TryGet(out _)));
    }

    [Fact]
    public void A_read_off_the_tick_thread_answers()
    {
        using var h = new KilnHarness();
        h.Step();
        h.Crosshair.Aim(new WorldPosition(5, 5, 5), entity: new EntityId(9));
        CrosshairHit got = default;
        var ok = false;
        var t = new Thread(() => ok = Crosshair.TryGet(out got));
        t.Start();
        t.Join();
        Assert.True(ok);
        Assert.Equal(new EntityId(9), got.Entity);
    }

    [Fact]
    public void The_coverage_table_has_a_row()
    {
        var row = Coverage.All.Single(r => r.Capabilities.Contains("kiln.client.crosshair"));
        Assert.Equal(CoverageKind.RecordingFake, row.Kind);
        Assert.Contains(Crosshair.Capability, row.Capabilities);
    }
}
