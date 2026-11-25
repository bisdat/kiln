// SPDX-License-Identifier: MIT
// Kiln.Client's crosshair (Crosshair.cs) against a fake kiln.client.crosshair v1 table: the binding is
// kiln_client_crosshair.h byte for byte, each native answer becomes the documented value (flags to optional fields,
// reasons, freshness), BUSY is asked again, and the capability rules (Read throws naming the row, TryGet never throws).
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Api.Tests;

public sealed unsafe class CrosshairTests : IDisposable
{
    private readonly IKilnBackend _previous = Backend.Current;
    private readonly BuildingInputTests.FakeBackend _backend = new();

    public CrosshairTests()
    {
        Backend.Current = _backend;
        FakeCrosshairTable.Reset();
        _backend.Tables[CrosshairNative.Service] = FakeCrosshairTable.Create();
        _backend.Features.Add(Crosshair.Capability);
    }

    public void Dispose() => Backend.Current = _previous;

    private static int Offset<T>(T* start, void* field) where T : unmanaged => (int)((byte*)field - (byte*)start);

    [Fact]
    public void The_binding_is_kiln_client_crosshair_h_byte_for_byte()
    {
        Assert.Equal(88, sizeof(CrosshairNative.Hit));
        Assert.Equal(16, sizeof(CrosshairNative.Table));
        var h = default(CrosshairNative.Hit);
        Assert.Equal(4, Offset(&h, &h.Flags));
        Assert.Equal(8, Offset(&h, &h.Status));
        Assert.Equal(12, Offset(&h, &h.Entity));
        Assert.Equal(16, Offset(&h, &h.Material));
        Assert.Equal(20, Offset(&h, &h.ClientSession));
        Assert.Equal(24, Offset(&h, &h.Sequence));
        Assert.Equal(32, Offset(&h, &h.ClientStep));
        Assert.Equal(40, Offset(&h, &h.AgeNs));
        Assert.Equal(48, Offset(&h, h.Position));
        Assert.Equal(72, Offset(&h, h.Normal));
        Assert.Equal(84, Offset(&h, &h.Distance));
        var t = default(CrosshairNative.Table);
        Assert.Equal(8, Offset(&t, &t.Get));
        Assert.Equal(new uint[] { 0, 1, 2, 3, 4 }, Enum.GetValues<CrosshairReason>().Select(v => (uint)v));
        Assert.Equal(TimeSpan.FromMilliseconds(250), Crosshair.FreshFor);
    }

    [Fact]
    public void A_terrain_hit_has_position_normal_distance_and_material_and_no_entity()
    {
        FakeCrosshairTable.Next = FakeCrosshairTable.Terrain(sequence: 41, ageNs: 3_000_000);
        var hit = Crosshair.Read(out var reason);
        Assert.Equal(CrosshairReason.None, reason);
        Assert.NotNull(hit);
        var h = hit!.Value;
        Assert.True(h.IsHit);
        Assert.True(h.IsFresh);
        Assert.Equal(new WorldPosition(1024.5, 37.25, -2048.75), h.Position);
        Assert.Equal(Vector3.UnitY, h.Normal);
        Assert.Equal(6f, h.Distance);
        Assert.Equal((byte)37, h.VoxelMaterial);
        Assert.Equal(EntityId.None, h.Entity);
        Assert.False(h.HasEntity);
        Assert.Equal(0u, h.GameStatus);
        Assert.Equal((41ul, 3u, 900ul), (h.Sequence, h.ClientSession, h.ClientStep));
        Assert.Equal(TimeSpan.FromMilliseconds(3), h.Age);
        Assert.Equal(88u, FakeCrosshairTable.LastSize);
    }

    [Fact]
    public void An_entity_hit_has_the_entity_and_a_field_without_its_flag_is_null()
    {
        var n = FakeCrosshairTable.Terrain(sequence: 2, ageNs: 0);
        n.Flags = 1 | 2 | 16;  // HIT | POSITION | ENTITY
        n.Entity = 4711;
        FakeCrosshairTable.Next = n;
        Assert.True(Crosshair.TryGet(out var h));
        Assert.True(h.IsHit && h.HasEntity);
        Assert.Equal(new EntityId(4711), h.Entity);
        Assert.NotNull(h.Position);
        Assert.Null(h.Normal);
        Assert.Null(h.Distance);
        Assert.Null(h.VoxelMaterial);
    }

    [Fact]
    public void A_miss_is_a_sample_that_hit_nothing()
    {
        FakeCrosshairTable.Next = new CrosshairNative.Hit { Size = 88, Status = 31, Sequence = 5, ClientSession = 3, ClientStep = 10 };
        Assert.True(Crosshair.TryGet(out var h));
        Assert.False(h.IsHit);
        Assert.Equal(31u, h.GameStatus);
        Assert.Null(h.Position);
        Assert.Null(h.Distance);
        Assert.Equal(EntityId.None, h.Entity);
    }

    [Fact]
    public void A_stale_sample_is_read_but_not_got()
    {
        FakeCrosshairTable.Next = FakeCrosshairTable.Terrain(sequence: 9, ageNs: 400_000_000);
        FakeCrosshairTable.Reason = (uint)CrosshairReason.Stale;
        var hit = Crosshair.Read(out var reason);
        Assert.Equal(CrosshairReason.Stale, reason);
        Assert.NotNull(hit);
        Assert.False(hit!.Value.IsFresh);
        Assert.Equal(TimeSpan.FromMilliseconds(400), hit.Value.Age);
        Assert.False(Crosshair.TryGet(out var got));
        Assert.Equal(default, got);
    }

    [Theory]
    [InlineData(CrosshairReason.NoSample)]
    [InlineData(CrosshairReason.Faulted)]
    public void No_sample_or_a_fault_reads_null(CrosshairReason why)
    {
        FakeCrosshairTable.Next = new CrosshairNative.Hit { Size = 88 };
        FakeCrosshairTable.Reason = (uint)why;
        Assert.Null(Crosshair.Read(out var reason));
        Assert.Equal(why, reason);
        Assert.False(Crosshair.TryGet(out _));
    }

    [Fact]
    public void Busy_is_asked_again_a_few_times()
    {
        FakeCrosshairTable.Next = FakeCrosshairTable.Terrain(sequence: 3, ageNs: 0);
        FakeCrosshairTable.BusyFor = 2;
        Assert.NotNull(Crosshair.Read(out var reason));
        Assert.Equal(CrosshairReason.None, reason);
        Assert.Equal(3, FakeCrosshairTable.Gets);

        FakeCrosshairTable.Gets = 0;
        FakeCrosshairTable.BusyFor = 100;
        Assert.Null(Crosshair.Read(out reason));
        Assert.Equal(CrosshairReason.Busy, reason);
        Assert.Equal(Crosshair.BusyRetries + 1, FakeCrosshairTable.Gets);
    }

    [Fact]
    public void The_row_off_throws_on_read_and_try_get_says_false()
    {
        _backend.Features.Remove(Crosshair.Capability);
        Assert.False(Crosshair.Available);
        var e = Assert.Throws<KilnUnavailableException>(() => Crosshair.Read(out _));
        Assert.Equal(Crosshair.Capability, e.Capability);
        Assert.False(Crosshair.TryGet(out _));
        Assert.Equal(0, FakeCrosshairTable.Gets);
    }

    [Fact]
    public void Kiln_answering_unavailable_throws_naming_the_row()
    {
        FakeCrosshairTable.Rc = 3;
        var e = Assert.Throws<KilnUnavailableException>(() => Crosshair.Read(out _));
        Assert.Equal(Crosshair.Capability, e.Capability);
        Assert.False(Crosshair.TryGet(out _));
    }

    [Fact]
    public void No_table_is_unavailable()
    {
        _backend.Tables.Remove(CrosshairNative.Service);
        Assert.False(Crosshair.Available);
        Assert.Throws<KilnUnavailableException>(() => Crosshair.Read(out _));
        Assert.False(Crosshair.TryGet(out _));
    }

    [Fact]
    public void Available_when_the_row_is_on_and_the_table_is_there() => Assert.True(Crosshair.Available);

    [Fact]
    public void The_surface_declares_its_side_thread_and_row()
    {
        Assert.True(Attribute.IsDefined(typeof(Crosshair), typeof(ClientOnlyAttribute)));
        Assert.True(Attribute.IsDefined(typeof(Crosshair), typeof(AnyThreadAttribute)));
        var read = typeof(Crosshair).GetMethod(nameof(Crosshair.Read))!;
        Assert.Equal(Crosshair.Capability, read.GetCustomAttributes(typeof(RequiresCapabilityAttribute), false).Cast<RequiresCapabilityAttribute>().Single().Name);
    }

    [Fact]
    public void A_read_from_another_thread_answers_too()
    {
        FakeCrosshairTable.Next = FakeCrosshairTable.Terrain(sequence: 77, ageNs: 0);
        CrosshairHit got = default;
        var ok = false;
        var t = new Thread(() => ok = Crosshair.TryGet(out got));
        t.Start();
        t.Join();
        Assert.True(ok);
        Assert.Equal(77ul, got.Sequence);
    }
}

/// <summary>A fake kiln.client.crosshair v1 table (kiln_client_crosshair.h). Its memory lives for the process.</summary>
internal static unsafe class FakeCrosshairTable
{
    public static CrosshairNative.Hit Next;
    public static uint Reason;
    public static int Rc, Gets, BusyFor;
    public static uint LastSize;

    public static void Reset()
    {
        Next = default;
        Reason = 0;
        Rc = Gets = BusyFor = 0;
        LastSize = 0;
    }

    public static CrosshairNative.Hit Terrain(ulong sequence, ulong ageNs)
    {
        var h = new CrosshairNative.Hit
        {
            Size = 88, Flags = 1 | 2 | 4 | 8 | 32, Status = 0, Material = 37, ClientSession = 3, Sequence = sequence,
            ClientStep = 900, AgeNs = ageNs, Distance = 6,
        };
        h.Position[0] = 1024.5;
        h.Position[1] = 37.25;
        h.Position[2] = -2048.75;
        h.Normal[1] = 1;
        return h;
    }

    public static nint Create()
    {
        var t = (CrosshairNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(CrosshairNative.Table));
        t->Header.Size = (uint)sizeof(CrosshairNative.Table);
        t->Header.Version = 1;
        t->Get = &Get;
        return (nint)t;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Get(CrosshairNative.Hit* out_, uint* reason)
    {
        Gets++;
        LastSize = out_->Size;
        if (out_->Size < 88) return 1;
        if (Rc != 0) return Rc;
        if (BusyFor > 0)
        {
            BusyFor--;
            *out_ = new CrosshairNative.Hit { Size = 88 };
            if (reason != null) *reason = 2;
            return 0;
        }
        *out_ = Next;
        out_->Size = 88;
        if (reason != null) *reason = Reason;
        return 0;
    }
}
