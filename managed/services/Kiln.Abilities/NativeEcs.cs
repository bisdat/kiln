// SPDX-License-Identifier: MIT
// NativeContext's reads and writes of other entities (since 0.2.0-dev.29, chapter 06c 9.1, S5): through kiln.abilities'
// native_ecs_read / native_ecs_write, which use kiln.world's view of impact_simulation and the job's own accessors,
// storage and tracker inside the native's call. Writes are queued from the call (any thread) and raised on the tick
// thread as one ComponentWrittenEvent per step, entity and component (first "before", last "after", every writer once),
// the way Kiln.Systems raises a program wrap's.
using System.Collections.Concurrent;
using System.Text;

namespace Kiln;

internal static unsafe class NativeEcs
{
    private static class Of<T> where T : unmanaged, IGameComponent<T>
    {
        public static readonly byte[] Name = Encoding.UTF8.GetBytes(T.Name);
        public static readonly bool Attribute = typeof(T).GetInterfaces()
            .Any(i => i.IsGenericType && i.GetGenericTypeDefinition() == typeof(IGameAttribute<,>));
        public static readonly bool Sized = T.Size == sizeof(T);
    }

    internal static bool Read(NativeCall call, ReadOnlySpan<byte> component, EntityId entity, Span<byte> into)
    {
        var t = AbilitiesNative.V2(nameof(AbilitiesNative.TableV2.NativeEcsWrite));
        if (t == null || entity.IsNone || call.Record == null || into.IsEmpty) return false;
        fixed (byte* n = component)
        fixed (byte* o = into)
            return t->NativeEcsRead(call.Record, new NativeStr { Ptr = n, Len = (nuint)component.Length }, entity.Value, o, (uint)into.Length) == AbilitiesNative.Ok;
    }

    internal static bool TryRead<T>(NativeCall call, EntityId entity, out T value) where T : unmanaged, IGameComponent<T>
    {
        value = default;
        if (!Of<T>.Sized) return false;
        fixed (T* v = &value)
            return Read(call, Of<T>.Name, entity, new Span<byte>(v, sizeof(T)));
    }

    internal static bool TryWrite<T>(NativeCall call, EntityId entity, in T value) where T : unmanaged, IGameComponent<T>
    {
        var t = AbilitiesNative.V2(nameof(AbilitiesNative.TableV2.NativeEcsWrite));
        if (t == null || entity.IsNone || call.Record == null || !Of<T>.Sized) return false;
        var size = sizeof(T);
        var before = new byte[size];
        var after = new byte[size];
        int rc;
        fixed (byte* n = Of<T>.Name)
        fixed (T* v = &value)
        fixed (byte* b = before)
        fixed (byte* a = after)
            rc = t->NativeEcsWrite(call.Record, new NativeStr { Ptr = n, Len = (nuint)Of<T>.Name.Length }, entity.Value, v, (uint)size,
                Of<T>.Attribute ? AbilitiesNative.EcsAttribute : 0, b, a);
        if (rc != AbilitiesNative.Ok) return false;
        if (!before.AsSpan().SequenceEqual(after))
        {
            var mod = call.Owner?.Info.Id ?? "kiln";
            NativeWrites.Add(new NativeWrites.Write(AbilitiesDispatch.Step, entity.Value, T.Name, before, after,
                $"{mod}/native:{Abilities.Natives.CachedName(call.Native)}", mod));
        }
        return true;
    }
}

/// <summary>NativeContext's changes of other entities, per step, entity and component: queued in the native's call,
/// raised as <see cref="ComponentWrittenEvent"/> on the tick thread (first "before", last "after", every writer once).</summary>
internal static class NativeWrites
{
    internal sealed record Write(ulong Step, uint Entity, string Component, byte[] Before, byte[] After, string Writer, string ModId)
    {
        internal long Session { get; } = Lifecycle.ServerSession;
    }

    private const int Most = 65536;
    private static readonly ConcurrentQueue<Write> Queue = new();
    private static int _count, _dropped;

    internal static void Add(Write w)
    {
        if (Interlocked.Increment(ref _count) > Most)
        {
            Interlocked.Decrement(ref _count);
            if (Interlocked.Exchange(ref _dropped, 1) == 0)
                ModLog.Warn($"abilities: more than {Most} NativeContext writes wait for the tick thread; later ones raise no event");
            return;
        }
        Queue.Enqueue(w);
    }

    /// <summary>Tick thread (kiln.abilities' listener): one event per step, entity and component.</summary>
    internal static void Flush()
    {
        if (Queue.IsEmpty) return;
        var groups = new Dictionary<(ulong, uint, string), Group>();
        var order = new List<Group>();
        while (Queue.TryDequeue(out var w))
        {
            Interlocked.Decrement(ref _count);
            if (w.Session != Lifecycle.ServerSession) continue;
            if (!groups.TryGetValue((w.Step, w.Entity, w.Component), out var g))
            {
                g = new Group(w);
                groups.Add((w.Step, w.Entity, w.Component), g);
                order.Add(g);
            }
            else
            {
                g.After = w.After;
                if (!g.Writers.Contains(w.Writer)) g.Writers.Add(w.Writer);
            }
        }
        Volatile.Write(ref _dropped, 0);
        if (!Events.HasSubscribers<ComponentWrittenEvent>()) return;
        foreach (var g in order)
            Events.Post(ComponentWrittenEvent.FromWrap(new EntityId(g.First.Entity), g.First.Component, g.First.Before, g.After,
                g.Writers, g.First.ModId));
    }

    /// <summary>Tests: how many wait, and forget them.</summary>
    internal static int Waiting => Volatile.Read(ref _count);

    internal static void Reset()
    {
        while (Queue.TryDequeue(out _)) { }
        _count = 0;
        _dropped = 0;
    }

    private sealed class Group(Write first)
    {
        public readonly Write First = first;
        public byte[] After = first.After;
        public readonly List<string> Writers = [first.Writer];
    }
}
