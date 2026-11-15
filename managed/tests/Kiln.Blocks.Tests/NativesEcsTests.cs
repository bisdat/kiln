// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>06c S5: NativeContext's reads and writes of other entities over a fake kiln.abilities table (the core's
/// native_ecs_read / native_ecs_write), the coalesced ComponentWrittenEvent, and the abilities.natives.ecs row.</summary>
[Collection("raw gates")]
public sealed unsafe class NativesEcsTests
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Hp : IGameAttribute<Hp, int>
    {
        public uint Root;
        public ushort Offset;
        public byte Flags, Count;
        public int Current, Max;
        public static uint Hash => 1;
        public static string Name => "Health";
        public static string QualifiedName => "keen::ecs::Health";
        public static int Size => 16;
        public static ulong Layout => 0;
        public static Span<int> ValuesOf(ref Hp self) => MemoryMarshal.CreateSpan(ref self.Current, 2);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Tag : IGameComponent<Tag>
    {
        public uint Value;
        public static uint Hash => 2;
        public static string Name => "SpawnTime";
        public static string QualifiedName => "keen::ecs::SpawnTime";
        public static int Size => 4;
        public static ulong Layout => 0;
    }

    // the fake core: entity 7's Health, readable and writable; nothing else
    static readonly byte[] Store = new byte[16];
    static uint _lastFlags;

    static string Name(NativeStr s) => System.Text.Encoding.UTF8.GetString(s.Ptr, (int)s.Len);

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    static int FakeRead(AbilitiesNative.NativeCall* c, NativeStr component, uint entity, void* outp, uint size)
    {
        if (Name(component) != "Health" || entity != 7) return AbilitiesNative.NotFound;
        if (size > 16) return AbilitiesNative.InvalidArgument;
        Store.AsSpan(0, (int)size).CopyTo(new Span<byte>(outp, (int)size));
        return AbilitiesNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    static int FakeWrite(AbilitiesNative.NativeCall* c, NativeStr component, uint entity, void* value, uint size,
        uint flags, void* before, void* after)
    {
        if (Name(component) != "Health" || entity != 7) return AbilitiesNative.NotFound;
        if (size != 16) return AbilitiesNative.InvalidArgument;
        _lastFlags = flags;
        Store.CopyTo(new Span<byte>(before, 16));
        new ReadOnlySpan<byte>(value, 16).CopyTo(Store);
        Store.CopyTo(new Span<byte>(after, 16));
        return AbilitiesNative.Ok;
    }

    static void WithTable(Action body)
    {
        var table = (AbilitiesNative.TableV2*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.TableV2));
        table->V1.Header.Size = (uint)sizeof(AbilitiesNative.TableV2);
        table->NativeEcsRead = &FakeRead;
        table->NativeEcsWrite = &FakeWrite;
        var field = typeof(AbilitiesNative).GetField("_t", BindingFlags.Static | BindingFlags.NonPublic)!;
        var previous = field.GetValue(null);
        try
        {
            field.SetValue(null, Pointer.Box(table, typeof(AbilitiesNative.Table*)));
            body();
        }
        finally
        {
            field.SetValue(null, previous);
            NativeMemory.Free(table);
        }
    }

    static ModContext NewMod(string id) => HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", "no-such-folder"), 0, "no-such-folder");

    [Fact]
    public void Reads_and_writes_go_through_the_core_and_a_change_raises_one_event_per_step_entity_and_component()
    {
        NativeWrites.Reset();
        var mod = NewMod("ecs-mod");
        var raw = (AbilitiesNative.NativeCall*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.NativeCall));
        raw->Size = (uint)sizeof(AbilitiesNative.NativeCall);
        raw->Hash = Abilities.HashOf("ecs-mod.OnHit");
        // the name RegisterSync would have recorded for the hash
        var names = (System.Collections.Concurrent.ConcurrentDictionary<uint, string>)typeof(Abilities.Natives)
            .GetField("Names", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
        names[raw->Hash] = "ecs-mod.OnHit";
        var events = new List<ComponentWrittenEvent>();
        using var sub = Events.On<ComponentWrittenEvent>(events.Add);
        try
        {
            WithTable(() =>
            {
                BitConverter.GetBytes(40).CopyTo(Store, 8);
                var ctx = new NativeContext(new NativeCall(raw, mod, raw: false, sync: true));
                Assert.True(ctx.TryRead<Hp>(new EntityId(7), out var hp));
                Assert.Equal(40, hp.Current);
                Assert.False(ctx.TryRead<Hp>(new EntityId(8), out _));        // an entity without it
                Assert.False(ctx.TryRead<Tag>(new EntityId(7), out _));       // a type the program does not declare
                Assert.False(ctx.TryRead<Hp>(EntityId.None, out _));
                hp.Current = 30;
                Assert.True(ctx.TryWrite(new EntityId(7), in hp));
                Assert.Equal(1u, _lastFlags);                                 // an attribute: the formulas run
                hp.Current = 20;
                Assert.True(ctx.TryWrite(new EntityId(7), in hp));            // the same step, entity and component
                Assert.True(ctx.TryWrite(new EntityId(7), in hp));            // unchanged: nothing queued
                Assert.False(ctx.TryWrite(new EntityId(7), new Tag()));
                Assert.Equal(2, NativeWrites.Waiting);
                NativeWrites.Flush();
            });
            var e = Assert.Single(events);
            Assert.Equal(("Health", 7u), (e.Component, e.Entity.Value));
            Assert.Equal(40, BitConverter.ToInt32(e.Result.Before.Span[8..]));      // the first write's before
            Assert.Equal(20, BitConverter.ToInt32(e.Result.After.Span[8..]));       // the last write's after
            Assert.Equal("ecs-mod/native:ecs-mod.OnHit", e.Writer);
            Assert.Equal(["ecs-mod/native:ecs-mod.OnHit"], e.Writers);
        }
        finally
        {
            NativeMemory.Free(raw);
            NativeWrites.Reset();
        }
    }

    [Fact]
    public void Without_the_bridge_reads_and_writes_are_false_with_no_access()
    {
        var raw = (AbilitiesNative.NativeCall*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.NativeCall));
        try
        {
            var ctx = new NativeContext(new NativeCall(raw, null, raw: false, sync: true));
            Assert.False(ctx.TryRead<Hp>(new EntityId(7), out _));  // no table here
            Assert.False(ctx.TryWrite(new EntityId(7), new Hp()));
            Assert.Equal(EntityId.None, ctx.Root);
        }
        finally
        {
            NativeMemory.Free(raw);
        }
    }

    [Fact]
    public void The_ecs_row_follows_the_worlds_self_check()
    {
        Assert.Equal(CapabilityState.Available, AbilitiesCaps.DecideEcs(true, true, AbilitiesNative.EcsOn, 0, CapabilityState.Available).State);
        Assert.Equal(CapabilityState.Pending, AbilitiesCaps.DecideEcs(true, true, AbilitiesNative.EcsPending, 0, CapabilityState.Available).State);
        var counts = AbilitiesCaps.DecideEcs(true, true, AbilitiesNative.EcsOff, AbilitiesNative.EcsReasonCounts, CapabilityState.Available);
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.Faulted), (counts.State, counts.Reason));
        Assert.Contains("job counts", counts.Text);
        Assert.Contains("native registry", AbilitiesCaps.DecideEcs(true, true, AbilitiesNative.EcsOff, AbilitiesNative.EcsReasonRegistry, CapabilityState.Available).Text);
        Assert.Equal(CapabilityReason.ServiceOlder, AbilitiesCaps.DecideEcs(true, false, null, 0, CapabilityState.Available).Reason);
        Assert.Equal(CapabilityState.Unavailable, AbilitiesCaps.DecideEcs(true, true, AbilitiesNative.EcsOn, 0, CapabilityState.Unavailable).State);
        Assert.Equal(CapabilityReason.ServiceMissing, AbilitiesCaps.DecideEcs(false, false, null, 0, CapabilityState.Available).Reason);
    }
}
