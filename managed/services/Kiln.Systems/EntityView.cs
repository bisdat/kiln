// SPDX-License-Identifier: MIT
// Per entity inside a stage: EntityView (the entity's components through the program's own slots), Attribute32, and what
// a stage writes through a view: marked for the save and replication on first use, compared when the stage is done with
// the entity, raised once per step per entity and component as Kiln.World's ComponentWrittenEvent (chapter 06a 9.12).
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;

namespace Kiln;

/// <summary>A per-entity stage with the entity's components (<see cref="EntityView"/>): runs once per entity of each
/// slice, on the thread that runs the slice.</summary>
public delegate void EntityViewHandler(ref EntityView entity);

/// <summary>One entity of a slice inside a per-entity stage, and its components through the program's own slots: the
/// components the program's descriptor lists (<see cref="SystemInfo.Writes"/> for the written ones). Valid during the
/// call only; on the slice's thread. A write through <see cref="Write{T}"/> or <see cref="Attribute"/> is marked for the
/// save and replication (once per entity and component) and raises <see cref="ComponentWrittenEvent"/> in the next step
/// when bytes changed. Not for programs whose descriptor uses whole-world lists (kiln.world has no slots for them: every
/// accessor throws).</summary>
public unsafe ref struct EntityView
{
    private readonly SystemsNative.Slice* _slice;
    private readonly ulong* _iter;
    private readonly ViewContext _ctx;
    private readonly uint _entity;
    private readonly int _row;

    internal EntityView(SystemsNative.Slice* slice, ulong* iter, uint entity, int row, ViewContext ctx)
    {
        _slice = slice;
        _iter = iter;
        _entity = entity;
        _row = row;
        _ctx = ctx;
    }

    /// <summary>The entity.</summary>
    public readonly EntityId Entity => new(_entity);

    /// <summary>Its row in the slice (from 0).</summary>
    public readonly int Row => _row;

    /// <summary>The world's step.</summary>
    public readonly ulong Step => _slice->Step;

    /// <summary>True when the program lists <paramref name="component"/> (any read or write list, or a singleton) and the
    /// entity has it now: an optional slot may be absent. Never throws for a name the program does not list.</summary>
    public readonly bool Has(string component)
    {
        if (string.IsNullOrEmpty(component)) return false;
        var at = _ctx.Lists.Find(component, write: false);
        if (at < 0) return false;
        var t = SystemsNative.T;
        uint size = 0;
        byte writable = 0;
        return t != null && t->GameSliceSlot(_slice, _iter, at >> 16, at & 0xFFFF, &size, &writable) != null;
    }

    /// <summary>The entity's <paramref name="component"/> to read (<c>"CurrentTransform"</c>, or a singleton such as
    /// <c>"impact::AttributeRegistry"</c>): a reference into the game's storage.</summary>
    /// <typeparam name="T">The component's layout (Kiln.Game's structs, <c>Kiln.Game.CurrentTransform</c>); at most the
    /// component's reflected size.</typeparam>
    /// <exception cref="InvalidOperationException">The program does not list it; an optional one the entity lacks;
    /// <typeparamref name="T"/> is larger than the component on this build.</exception>
    public readonly ref readonly T Read<T>(string component) where T : unmanaged =>
        ref Unsafe.AsRef<T>(Need(component, false, sizeof(T), out _, out _, out _));

    /// <summary>The entity's <paramref name="component"/> to write (a write list of the program): a reference into the
    /// game's storage. The first use per entity and component in the stage marks it for the save and replication (the
    /// game's own MarkWritten); the stage's change raises <see cref="ComponentWrittenEvent"/> in the next step.</summary>
    /// <typeparam name="T">The component's layout; at most the component's reflected size.</typeparam>
    /// <exception cref="InvalidOperationException">The program does not write it; an optional one the entity lacks;
    /// <typeparamref name="T"/> is larger than the component on this build.</exception>
    public readonly ref T Write<T>(string component) where T : unmanaged =>
        ref Unsafe.AsRef<T>(Need(component, true, sizeof(T), out _, out _, out _));

    /// <summary>An attribute component the program writes (<c>"Fuel"</c>, <c>"Health"</c>, <c>"Stamina"</c>): its slots,
    /// written like <see cref="Write{T}"/>; <see cref="Attribute32.Changed"/> runs the game's formulas after a change.</summary>
    /// <exception cref="ArgumentException">Not an attribute write slot, or its value storage is outside the component.</exception>
    /// <exception cref="InvalidOperationException">Attribute validation is unavailable on this build.</exception>
    public readonly Attribute32 Attribute(string component)
    {
        ArgumentException.ThrowIfNullOrEmpty(component);
        var t = SystemsNative.T;
        var at = _ctx.Lists.Find(component, write: true);
        if (at < 0) throw new ArgumentException($"{component} is not an attribute write slot", nameof(component));
        var rc = t == null ? SystemsNative.Unavailable : t->GameSliceAttrValidate(_slice, _iter, at >> 16, at & 0xFFFF);
        if (rc == SystemsNative.InvalidArgument)
            throw new ArgumentException($"{component} is not an attribute or its block is malformed", nameof(component));
        if (rc != SystemsNative.Ok) throw new InvalidOperationException($"{component}: attribute validation unavailable ({rc})");
        var p = Need(component, true, 8, out var list, out var index, out var size);
        return new Attribute32(p, size, _slice, _iter, list, index, component);
    }

    /// <summary>The program whose slice this entity is of.</summary>
    public readonly string Program => _ctx.Program;

    // ---- by descriptor position: Kiln.Game's generated program views (Ecs.Programs.*) ----

    // A typed view of `program` with `contract` binds this entity: the slice must be of that program, and its live
    // descriptor (this world session's) must have the view's contract, so the view's positions name the right slots.
    internal readonly EntityView Expect(string program, uint contract)
    {
        if (!string.Equals(_ctx.Program, program, StringComparison.Ordinal))
            throw new InvalidOperationException($"a view of {program} cannot bind an entity of a {_ctx.Program} slice");
        var live = _ctx.Lists.Contract;
        if (live != contract)
            throw new InvalidOperationException(live is null
                ? $"{program}: kiln.world cannot read its descriptor's contract on this build; typed views are off"
                : $"{program}: the live descriptor's contract is 0x{live:X8}, the view's 0x{contract:X8} (made for another game build)");
        return this;
    }

    internal readonly bool HasAt(int list, int index)
    {
        var t = SystemsNative.T;
        uint size = 0;
        byte writable = 0;
        return t != null && t->GameSliceSlot(_slice, _iter, list, index, &size, &writable) != null;
    }

    internal readonly ref readonly T ReadAt<T>(int list, int index, string component) where T : unmanaged =>
        ref Unsafe.AsRef<T>(Slot(list, index, component, false, sizeof(T), out _));

    internal readonly ref T WriteAt<T>(int list, int index, string component) where T : unmanaged =>
        ref Unsafe.AsRef<T>(Slot(list, index, component, true, sizeof(T), out _));

    internal readonly Attribute32 AttributeAt(int list, int index, string component)
    {
        var t = SystemsNative.T;
        var rc = t == null ? SystemsNative.Unavailable : t->GameSliceAttrValidate(_slice, _iter, list, index);
        if (rc == SystemsNative.InvalidArgument)
            throw new ArgumentException($"{component} is not an attribute or its block is malformed", nameof(component));
        if (rc != SystemsNative.Ok) throw new InvalidOperationException($"{component}: attribute validation unavailable ({rc})");
        var p = Slot(list, index, component, true, 8, out var size);
        return new Attribute32(p, size, _slice, _iter, list, index, component);
    }

    // An attribute's values to read: validated as AttributeAt does, but not marked (a capture, a test before a write).
    internal readonly ReadOnlySpan<int> AttributeReadAt(int list, int index, string component)
    {
        var t = SystemsNative.T;
        var rc = t == null ? SystemsNative.Unavailable : t->GameSliceAttrValidate(_slice, _iter, list, index);
        if (rc == SystemsNative.InvalidArgument)
            throw new ArgumentException($"{component} is not an attribute or its block is malformed", nameof(component));
        if (rc != SystemsNative.Ok) throw new InvalidOperationException($"{component}: attribute validation unavailable ({rc})");
        var p = Slot(list, index, component, false, 8, out var size);
        return new Attribute32(p, size, _slice, _iter, list, index, component).Values;
    }

    private readonly byte* Need(string component, bool write, int want, out int list, out int index, out int size)
    {
        ArgumentException.ThrowIfNullOrEmpty(component);
        var t = SystemsNative.T;
        var at = _ctx.Lists.Find(component, write);
        if (at < 0 || t == null)
            throw new InvalidOperationException(write
                ? $"{_ctx.Program} does not write {component} (its write lists: {_ctx.Lists.Describe(true)})"
                : $"{_ctx.Program} does not list {component} (its lists: {_ctx.Lists.Describe(false)})");
        list = at >> 16;
        index = at & 0xFFFF;
        return Slot(list, index, component, write, want, out size);
    }

    private readonly byte* Slot(int list, int index, string component, bool write, int want, out int size)
    {
        var t = SystemsNative.T;
        if (t == null) throw new InvalidOperationException($"{_ctx.Program}: kiln.world is not here");
        if (write && list is not (SystemsNative.ListWrite or SystemsNative.ListWriteOpt))
            throw new InvalidOperationException($"{_ctx.Program}: {component} is not one of its write slots");
        uint reflected = 0;
        byte writable = 0;
        var p = (byte*)t->GameSliceSlot(_slice, _iter, list, index, &reflected, &writable);
        if (p == null)
            throw new InvalidOperationException(list is SystemsNative.ListTplOpt or SystemsNative.ListReadOpt or SystemsNative.ListWriteOpt
                ? $"{Entity} has no {component} (an optional component of {_ctx.Program}): ask Has first"
                : $"{_ctx.Program}: kiln.world has no slot for {component} on this build (kiln.log says why)");
        if (reflected == 0)
            throw new InvalidOperationException($"{component}: its size is not known on this build; access is unsafe");
        if (want > reflected)
            throw new InvalidOperationException($"{component} is {reflected} bytes on this build; the layout asked is {want}");
        size = reflected != 0 ? (int)reflected : want;
        if (write) _ctx.Touch(t, _slice, _iter, _entity, list, index, p, size, component);
        return p;
    }
}

/// <summary>An attribute component (Health, Stamina, Mana, Fuel, Level ...) through a program wrap's write slot: the
/// game's block <c>{u32 root id, u16 storage offset, u8 flags (bit 0: has values), u8 count}</c> and its signed 32-bit
/// slots. Which slot is which is the attribute's own: Fuel <c>[0]</c> current units, <c>[1]</c> the maximum; Health
/// <c>[0]</c> current, <c>[1]</c> minimum, <c>[7]</c> maximum (docs/re/combat.md, docs/re/items.md). Valid during the
/// per-entity call only.</summary>
public unsafe ref struct Attribute32
{
    private readonly byte* _p;
    private readonly int _size;
    private readonly SystemsNative.Slice* _slice;
    private readonly ulong* _iter;
    private readonly int _list, _index;

    internal Attribute32(byte* p, int size, SystemsNative.Slice* slice, ulong* iter, int list, int index, string component)
    {
        _p = p;
        _size = size;
        _slice = slice;
        _iter = iter;
        _list = list;
        _index = index;
        Component = component;
    }

    /// <summary>The component's name.</summary>
    public string Component { get; }

    /// <summary>The attribute group's root id (the block's first word).</summary>
    public readonly uint RootId => Unsafe.ReadUnaligned<uint>(_p);

    /// <summary>True when the block holds values (flag bit 0): the game skips an attribute without them.</summary>
    public readonly bool HasValues => (_p[6] & 1) != 0;

    private readonly int StorageOffset => Unsafe.ReadUnaligned<ushort>(_p + 4);

    /// <summary>How many slots (the block's count, within the component's size).</summary>
    public readonly int Count => HasValues ? Math.Max(0, Math.Min(_p[7], (_size - StorageOffset) / 4)) : 0;

    /// <summary>The slots.</summary>
    public readonly Span<int> Values => new(_p + (HasValues ? StorageOffset : 8), Count);

    /// <summary>Slot <paramref name="slot"/>.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="slot"/> is not below <see cref="Count"/>.</exception>
    public readonly ref int this[int slot]
    {
        get
        {
            if ((uint)slot >= (uint)Count) throw new ArgumentOutOfRangeException(nameof(slot), $"{Component} has {Count} slots");
            return ref ((int*)(_p + StorageOffset))[slot];
        }
    }

    /// <summary>Runs the game's attribute formulas on the component after you changed a slot (clamps to the maximum,
    /// derived values), with the program's own <c>impact::AttributeRegistry</c>, as the game's own writers do.</summary>
    /// <exception cref="InvalidOperationException">The program does not declare <c>impact::AttributeRegistry</c>, the
    /// block holds no values, the formulas are not known on this build, or they faulted.</exception>
    public readonly void Changed()
    {
        var t = SystemsNative.T;
        var rc = t == null ? SystemsNative.Unavailable : t->GameSliceAttrChanged(_slice, _iter, _list, _index);
        switch (rc)
        {
            case SystemsNative.Ok:
                return;
            case SystemsNative.InvalidArgument:
                throw new InvalidOperationException($"{Component}: the game's attribute formulas need a block with values and " +
                    "impact::AttributeRegistry in the program's lists");
            case SystemsNative.Internal:
                throw new InvalidOperationException($"{Component}: the game's attribute formulas faulted");
            default:
                throw new InvalidOperationException($"{Component}: the game's attribute formulas are not known on this build");
        }
    }
}

/// <summary>A program's descriptor lists by name (kiln.world's directory), per world kind and program name; and the
/// lookups of a component name in them.</summary>
internal sealed unsafe class ProgramLists
{
    private static readonly ConcurrentDictionary<(WorldKind, ulong, string), ProgramLists> Cache = new();
    private static readonly int[] ReadOrder =
    [
        SystemsNative.ListTpl, SystemsNative.ListTplOpt, SystemsNative.ListRead, SystemsNative.ListReadOpt,
        SystemsNative.ListWrite, SystemsNative.ListWriteOpt, SystemsNative.ListSysRead, SystemsNative.ListSysWrite,
    ];
    private static readonly int[] WriteOrder = [SystemsNative.ListWrite, SystemsNative.ListWriteOpt];

    private readonly string[][] _names = new string[SystemsNative.Lists][];
    private readonly ConcurrentDictionary<string, int> _read = new(), _write = new();
    private readonly WorldKind _world;
    private readonly int _index;
    private uint _contract;
    private int _contractState;  // 0 not asked, 1 _contract holds the live one, 2 kiln.world could not say

    private ProgramLists(SystemsNative.Table* t, WorldKind world, int index)
    {
        _world = world;
        _index = index;
        var buffer = stackalloc NativeStr[64];
        var complete = t != null;
        for (var list = 0; list < SystemsNative.Lists; list++)
        {
            var n = t == null ? 0 : t->GameProgramList((uint)world, index, list, buffer, 64);
            if (n < 0) complete = false;
            var names = new string[Math.Clamp(n, 0, 64)];
            for (var i = 0; i < names.Length; i++) names[i] = buffer[i].ToString();
            _names[list] = names;
        }
        Complete = complete;
    }

    /// <summary>False when kiln.world could not answer for one of the lists (no directory at this moment: a read inside
    /// the game's step that gave up after its bound, a world going away). Never kept in the session's cache.</summary>
    internal bool Complete { get; }

    internal static ProgramLists Of(SystemsNative.Table* t, WorldKind world, string program, int index)
    {
        var session = t == null ? 0 : t->GameWorldSession((uint)world);
        if (Cache.TryGetValue((world, session, program), out var lists)) return lists;
        lists = new ProgramLists(t, world, index);
        return lists.Complete ? Cache.GetOrAdd((world, session, program), lists) : lists;
    }

    /// <summary>Tests: forget every program.</summary>
    internal static void Reset() => Cache.Clear();

    /// <summary>The live descriptor's contract in this world session (kiln.world's game_program_contract, asked until it
    /// answers), or null when kiln.world cannot say (<see cref="ContractUnread"/>: not now, asked again next time).</summary>
    internal uint? Contract
    {
        get
        {
            var state = Volatile.Read(ref _contractState);
            if (state == 0)
            {
                var t = SystemsNative.T;
                uint c = 0;
                var rc = SystemsNative.HasContract(t) ? t->GameProgramContract((uint)_world, _index, &c, null, 0, null) : SystemsNative.Unavailable;
                if (rc == SystemsNative.NotFound) return null;  // no directory at this moment: not an answer for the session
                _contract = c;
                state = rc == SystemsNative.Ok ? 1 : 2;
                Volatile.Write(ref _contractState, state);
            }
            return state == 1 ? _contract : null;
        }
    }

    /// <summary>True when the last <see cref="Contract"/> read had no answer (no directory at that moment).</summary>
    internal bool ContractUnread => Volatile.Read(ref _contractState) == 0;

    // (list << 16) | position, or -1
    internal int Find(string component, bool write)
    {
        var cache = write ? _write : _read;
        if (cache.TryGetValue(component, out var at)) return at;
        at = -1;
        foreach (var list in write ? WriteOrder : ReadOrder)
        {
            var names = _names[list];
            for (var i = 0; i < names.Length && at < 0; i++)
                if (Same(names[i], component)) at = (list << 16) | i;
            if (at >= 0) break;
        }
        cache.TryAdd(component, at);
        return at;
    }

    internal string Describe(bool write)
    {
        var all = new List<string>();
        foreach (var list in write ? WriteOrder : ReadOrder) all.AddRange(_names[list]);
        return all.Count == 0 ? "none" : string.Join(", ", all);
    }

    private const string EcsPrefix = "keen::ecs::";

    private static bool Same(string listed, string asked) =>
        listed == asked ||
        (asked.StartsWith(EcsPrefix, StringComparison.Ordinal) && listed.AsSpan().SequenceEqual(asked.AsSpan(EcsPrefix.Length))) ||
        (listed.StartsWith(EcsPrefix, StringComparison.Ordinal) && listed.AsSpan(EcsPrefix.Length).SequenceEqual(asked));
}

/// <summary>One per-entity stage call (one slice, one thread): the program's lists, and the writes made through views,
/// each marked once per entity and component and compared when the stage is done with the entity.</summary>
internal sealed unsafe class ViewContext
{
    [ThreadStatic] private static ViewContext? _spare;

    private struct Rec
    {
        public int List, Index, Size, At;
        public byte* Ptr;
        public string Component;
    }

    private Rec[] _recs = new Rec[8];
    private int _count;
    private byte[] _before = new byte[256];
    private int _used;
    private uint _entity;
    private bool _track;
    private ulong _step;
    private ManagedStage? _stage;

    internal ProgramLists Lists = null!;
    internal string Program = "";

    internal static ViewContext Begin(SystemsNative.Table* t, SystemsNative.Slice* s, ManagedStage stage)
    {
        var ctx = _spare ?? new ViewContext();
        _spare = null;
        ctx._stage = stage;
        ctx.Program = stage.Wrap.Program;
        ctx.Lists = ProgramLists.Of(t, stage.Wrap.World, stage.Wrap.Program, s->ProgramIndex);
        ctx._step = s->Step;
        // the change events: server worlds only (a client world's are chapter 14's), and only while someone listens
        // the bytes before are kept for ComponentWrittenEvent and, since 07 S7, for a component watch (its Kiln origin)
        ctx._track = stage.Wrap.World == WorldKind.Server && (Events.HasSubscribers<ComponentWrittenEvent>() || ComponentChanges.Watching);
        ctx._count = 0;
        ctx._used = 0;
        ctx._entity = 0;
        return ctx;
    }

    internal void End()
    {
        Done();
        _stage = null;
        _spare = this;
    }

    // A write through a view: mark once per entity and component, keep the bytes before.
    internal void Touch(SystemsNative.Table* t, SystemsNative.Slice* s, ulong* iter, uint entity, int list, int index,
        byte* p, int size, string component)
    {
        if (entity != _entity || _count == 0)
        {
            Done();
            _entity = entity;
        }
        for (var i = 0; i < _count; i++)
            if (_recs[i].List == list && _recs[i].Index == index) return;
        var rc = t->GameSliceMark(s, iter, list, index);
        if (rc == SystemsNative.InvalidArgument)
            throw new InvalidOperationException($"{Program}: {component} is not one of its write slots");
        if (rc != SystemsNative.Ok)
            throw new InvalidOperationException($"{Program}: {component} could not be marked for save and replication (result {rc})");
        if (_count == _recs.Length) Array.Resize(ref _recs, _count * 2);
        var at = _used;
        if (_track)
        {
            if (_used + size > _before.Length) Array.Resize(ref _before, Math.Max(_before.Length * 2, _used + size));
            new ReadOnlySpan<byte>(p, size).CopyTo(_before.AsSpan(_used));
            _used += size;
        }
        _recs[_count++] = new Rec { List = list, Index = index, Size = size, At = at, Ptr = p, Component = component };
    }

    // The stage is done with the entity: its changed components go to the tick thread.
    private void Done()
    {
        if (_track && _stage is { } stage)
            for (var i = 0; i < _count; i++)
            {
                ref var r = ref _recs[i];
                var now = new ReadOnlySpan<byte>(r.Ptr, r.Size);
                var before = _before.AsSpan(r.At, r.Size);
                if (now.SequenceEqual(before)) continue;
                WrapWrites.Add(new WrapWrites.Write(_step, _entity, ComponentName(r.Component), before.ToArray(), now.ToArray(),
                    stage.Wrap.Writer, stage.Wrap.ModId));
                ComponentChanges.NoteWrapWrite(_entity, ComponentName(r.Component), before, now, stage.Wrap.ModId);  // 07 S7
            }
        _count = 0;
        _used = 0;
    }

    private static string ComponentName(string asked) =>
        asked.StartsWith("keen::ecs::", StringComparison.Ordinal) ? asked["keen::ecs::".Length..] : asked;
}

/// <summary>The changes program wraps made, per step per entity and component: queued on the slice threads, raised as
/// <see cref="ComponentWrittenEvent"/> on the tick thread (first "before", last "after", every writer once).</summary>
internal static class WrapWrites
{
    internal sealed record Write(ulong Step, uint Entity, string Component, byte[] Before, byte[] After, string Writer, string ModId);

    private const int Most = 262144;
    private static readonly ConcurrentQueue<Write> Queue = new();
    private static int _count, _dropped;

    internal static void Add(Write w)
    {
        if (Interlocked.Increment(ref _count) > Most)
        {
            Interlocked.Decrement(ref _count);
            if (Interlocked.Exchange(ref _dropped, 1) == 0)
                Backend.Current.Log(0, LogLevel.Warn, $"Kiln.Systems: more than {Most} wrap writes wait for the tick thread; later ones raise no event");
            return;
        }
        Queue.Enqueue(w);
    }

    /// <summary>Tick thread (the wraps' service tick): one event per step, entity and component.</summary>
    internal static void Flush()
    {
        if (Queue.IsEmpty) return;
        var groups = new Dictionary<(ulong, uint, string), Group>();
        var order = new List<Group>();
        while (Queue.TryDequeue(out var w))
        {
            Interlocked.Decrement(ref _count);
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

    /// <summary>How many wait for the tick thread.</summary>
    internal static int Waiting => Volatile.Read(ref _count);

    /// <summary>Tests: forget what waits.</summary>
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
