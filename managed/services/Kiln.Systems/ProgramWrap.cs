// SPDX-License-Identifier: MIT
// A wrap on one program of one world, its managed stages (each a native kiln.world wrap), and what a stage sees.
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>What a slice stage gets: the slice (one archetype group's range of the program's entities), valid during
/// the call only. On the slice's thread.</summary>
public unsafe ref struct Slice
{
    private readonly SystemsNative.Slice* _native;
    private readonly ManagedStage _stage;

    internal Slice(SystemsNative.Slice* native, ManagedStage stage)
    {
        _native = native;
        _stage = stage;
    }

    /// <summary>Entities in the slice (0 for a SYSTEM program).</summary>
    public readonly int Count => (int)_native->Count;

    /// <summary>The world's step.</summary>
    public readonly ulong Step => _native->Step;

    /// <summary>True on the world's own thread, false on a game task worker.</summary>
    public readonly bool IsWorldThread => _native->ThreadIsWorld != 0;

    /// <summary>The wrap's world.</summary>
    public readonly WorldKind World => (WorldKind)_native->WorldKind;

    /// <summary>The program's name.</summary>
    public readonly string Program => _stage.Wrap.Program;

    /// <summary>The id of the slice's entity at <paramref name="row"/> (from 0).</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="row"/> is not within the slice.</exception>
    public readonly EntityId EntityAt(int row)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(row);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(row, Count);
        return new EntityId(_native->EntityIds[(long)row * (_native->EntityStride / 4)]);
    }

    /// <summary>Inside a Replace: runs the game's update for this slice, once.</summary>
    /// <exception cref="InvalidOperationException">Not a Replace, or it ran already.</exception>
    public readonly void RunOriginal()
    {
        var t = SystemsNative.T;
        var rc = t == null ? SystemsNative.Unavailable : t->GameSliceRunOriginal(_native);
        if (rc != SystemsNative.Ok)
            throw new InvalidOperationException("RunOriginal runs the game's update once, inside a Replace stage");
        ManagedStage.OriginalCalled = true;
    }

    /// <summary>Runs <paramref name="action"/> on the tick thread before the next step, in your mod (the only way to use a
    /// tick-thread API from a stage).</summary>
    public readonly void Post(Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        if (_stage.Wrap.Mod is { } mod) mod.Scheduler.Post(action);
        else ProgramWrap.PostToTick(action);
    }
}

/// <summary>A wrap on one program of one world (<see cref="Systems.Wrap"/>): its stages, their state and counters.
/// Dispose removes every stage.</summary>
public sealed unsafe class ProgramWrap : IDisposable
{
    private readonly WrapOptions _options;
    private readonly List<ManagedStage> _stages = new();
    private readonly object _lock = new();
    private Func<SliceInfo, bool>? _when;
    private sealed class ArchetypeFilter(uint[] has, uint[] lacks, string[] hasNames, string[] lacksNames)
    {
        internal readonly uint[] Has = has, Lacks = lacks;
        internal readonly string[] HasNames = hasNames, LacksNames = lacksNames;
        internal readonly System.Collections.Concurrent.ConcurrentDictionary<(ulong Session, nint Layout), bool> Cache = new();
    }
    private ArchetypeFilter? _archetype;
    private bool _disposed;

    internal ProgramWrap(string program, WrapOptions options, ModContext? mod)
    {
        Program = program;
        _options = options;
        Mod = mod;
        if (mod is not null) mod.Retiring += Retire;
    }

    /// <summary>The program's name.</summary>
    public string Program { get; }

    /// <summary>The wrap's world.</summary>
    public WorldKind World => _options.World;

    internal ModContext? Mod { get; }
    internal string ModId => Mod?.Info.Id ?? "kiln";
    private string? _writer;
    internal string Writer => _writer ??= $"{ModId}/wrap:{Program}";  // ComponentWrittenEvent.Writer

    /// <summary>Raised on the tick thread when one of the wrap's stages is turned off: a fault, its budget (1 ms per
    /// slice, 10 slices in a row over it), an exception, native SET.</summary>
    public event Action<WrapDisabledEvent>? Disabled;

    /// <summary>A stage before the game's update (priorities high to low; ties in the order made).</summary>
    [TickThreadOnly]
    public ProgramWrap Before(SliceHandler handler, int? priority = null) => Add(WrapStage.Before, handler, null, priority);

    /// <summary>A stage after the game's update (the exact reverse of the Before order).</summary>
    [TickThreadOnly]
    public ProgramWrap After(SliceHandler handler, int? priority = null) => Add(WrapStage.After, handler, null, priority);

    /// <summary>Per entity before the game's update: <paramref name="handler"/> runs for every entity of each slice.</summary>
    [TickThreadOnly]
    public ProgramWrap BeforeEach(EntityHandler handler, int? priority = null) =>
        Add(WrapStage.Before, null, handler ?? throw new ArgumentNullException(nameof(handler)), priority);

    /// <summary>Per entity after the game's update.</summary>
    [TickThreadOnly]
    public ProgramWrap AfterEach(EntityHandler handler, int? priority = null) =>
        Add(WrapStage.After, null, handler ?? throw new ArgumentNullException(nameof(handler)), priority);

    /// <summary>Per entity before the game's update, with the entity's components (<see cref="EntityView"/>): read what
    /// the program reads, write what it writes. Writes are marked for the save and replication and raise
    /// <see cref="ComponentWrittenEvent"/> (<c>Writer</c> <c>"&lt;modid&gt;/wrap:&lt;program&gt;"</c>) in the next step.</summary>
    [TickThreadOnly]
    public ProgramWrap BeforeEach(EntityViewHandler handler, int? priority = null) =>
        Add(WrapStage.Before, null, handler ?? throw new ArgumentNullException(nameof(handler)), priority);

    /// <summary>Per entity after the game's update, with the entity's components (see the other
    /// <see cref="BeforeEach(EntityViewHandler, int?)"/>).</summary>
    [TickThreadOnly]
    public ProgramWrap AfterEach(EntityViewHandler handler, int? priority = null) =>
        Add(WrapStage.After, null, handler ?? throw new ArgumentNullException(nameof(handler)), priority);

    // AroundEach (ProgramWrap<T>): a Before that captures and an After that applies, at one priority, off together.
    internal void Around(IAround around, int? priority)
    {
        Services.RequireTickThread("ProgramWrap.AroundEach");
        var before = Make(WrapStage.Before, null, around.CaptureView, priority, around);
        var after = Make(WrapStage.After, null, around.ApplyView, priority, around);
        before.Partner = after;
        after.Partner = before;
        if (before.State == WrapState.Refused) before.RaiseDisabled(before.Detail ?? "refused");
        if (after.State == WrapState.Refused) after.RaiseDisabled(after.Detail ?? "refused");
        if (before.State == WrapState.Refused || after.State == WrapState.Refused)
        {
            before.Unwrap();
            after.Unwrap();
        }
    }

    /// <summary>Instead of the game's update; call <see cref="Slice.RunOriginal"/> to run it. One per program.</summary>
    /// <returns><c>Ok = false</c> when another mod replaces the program (named in the reason).</returns>
    [TickThreadOnly]
    public WrapResult<ProgramWrap> Replace(SliceHandler handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        Services.RequireTickThread("ProgramWrap.Replace");
        foreach (var other in Systems.Live.Keys)
            if (other.Kind == WrapStage.Replace && other.Wrap.Program == Program && other.Wrap.World == World &&
                other.State is WrapState.Pending or WrapState.Active && !other.Removing)
                return new WrapResult<ProgramWrap>(false, $"{Program} is replaced by mod {other.Wrap.ModId} already", null);
        var stage = Make(WrapStage.Replace, handler, null, null);
        if (stage.State == WrapState.Refused)
            return new WrapResult<ProgramWrap>(false, stage.Detail ?? $"{Program}: the Replace was refused", null);
        return new WrapResult<ProgramWrap>(true, null, this);
    }

    /// <summary>Runs the stages only on slices <paramref name="filter"/> accepts (called per slice, on its thread).</summary>
    public ProgramWrap When(Func<SliceInfo, bool> filter)
    {
        ArgumentNullException.ThrowIfNull(filter);
        Volatile.Write(ref _when, filter);
        return this;
    }

    /// <summary>Runs the stages only on slices whose archetype has every component of <paramref name="has"/> and none of
    /// <paramref name="lacks"/>; decided once per archetype and world.</summary>
    /// <exception cref="ArgumentException">A name is null or empty.</exception>
    public ProgramWrap WhenArchetype(IEnumerable<string> has, IEnumerable<string>? lacks = null)
    {
        ArgumentNullException.ThrowIfNull(has);
        string[] h = [.. has], l = lacks is null ? [] : [.. lacks];
        var filter = new ArchetypeFilter(Hashes(h), Hashes(l), h, l);
        Volatile.Write(ref _archetype, filter);
        return this;
    }

    /// <summary>The wrap's state: <see cref="WrapState.Active"/> while a stage is in the chain, Pending until then,
    /// Removed after Dispose, Refused when every stage was refused.</summary>
    [AnyThread]
    public WrapState State
    {
        get
        {
            lock (_lock)
            {
                if (_disposed) return WrapState.Removed;
                if (_stages.Count == 0) return WrapState.Pending;
                if (_stages.Any(s => s.State == WrapState.Active)) return WrapState.Active;
                if (_stages.Any(s => s.State == WrapState.Pending)) return WrapState.Pending;
                return _stages.All(s => s.State == WrapState.Refused) ? WrapState.Refused : WrapState.Removed;
            }
        }
    }

    /// <summary>What the wrap's stages did so far.</summary>
    [AnyThread]
    public WrapStats Stats
    {
        get
        {
            long slices = 0, entities = 0, faults = 0, overruns = 0;
            double us = 0;
            var threads = 0;
            var t = SystemsNative.T;
            lock (_lock)
                foreach (var s in _stages)
                {
                    var i = s.Info(t);
                    slices += (long)i.Calls;
                    entities += (long)i.Entities;
                    faults += (long)i.Faults + s.ManagedFaults;
                    overruns += (long)i.Overruns;
                    us += i.TotalNs / 1000.0;
                    threads = Math.Max(threads, (int)i.Threads);
                }
            return new WrapStats(slices, entities, us, faults, overruns, threads);
        }
    }

    /// <summary>Removes every stage: no stage call starts after this returns.</summary>
    [TickThreadOnly]
    public void Dispose()
    {
        Services.RequireTickThread("ProgramWrap.Dispose");
        List<ManagedStage> stages;
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
            stages = [.. _stages];
        }
        foreach (var s in stages) s.Unwrap();
        if (Mod is not null) Mod.Retiring -= Retire;
    }

    // ---- inside ----

    private ProgramWrap Add(WrapStage kind, SliceHandler? slice, Delegate? each, int? priority)
    {
        if (slice is null && each is null) throw new ArgumentNullException(nameof(slice));
        Services.RequireTickThread($"ProgramWrap.{kind}");
        var stage = Make(kind, slice, each, priority);
        if (stage.State == WrapState.Refused) stage.RaiseDisabled(stage.Detail ?? "refused");
        return this;
    }

    private ManagedStage Make(WrapStage kind, SliceHandler? slice, Delegate? each, int? priority, IAround? around = null)
    {
        lock (_lock)
            if (_disposed) throw new ObjectDisposedException(nameof(ProgramWrap));
        if (Mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var off = Systems.WhyOff(World);
        var stage = new ManagedStage(this, kind, slice, each, priority ?? _options.Priority) { Around = around };
        if (off.Refuse is { } refuse)
        {
            stage.Refuse(refuse);
            lock (_lock) _stages.Add(stage);
            return stage;
        }
        stage.Install(Owner);
        lock (_lock) _stages.Add(stage);
        return stage;
    }

    private ulong Owner => Mod is { } m && m.Owner != 0 ? m.Owner : ProcessOwner;

    private static ulong _processOwner;
    private static ulong ProcessOwner => _processOwner != 0 ? _processOwner : _processOwner = Backend.Current.OwnerCreate("Kiln.Systems");

    private static uint[] Hashes(IEnumerable<string> names)
    {
        var list = new List<uint>();
        foreach (var n in names)
        {
            ArgumentException.ThrowIfNullOrEmpty(n);
            var q = n.Contains("::", StringComparison.Ordinal) ? n : "keen::ecs::" + n;
            var h = 0x811C9DC5u;
            foreach (var b in System.Text.Encoding.UTF8.GetBytes(q)) h = (h ^ b) * 0x01000193u;
            list.Add(h);
        }
        return [.. list];
    }

    // The filters, per slice: When on every call, WhenArchetype cached per archetype. `off`: WhenArchetype cannot decide
    // on this build or world (no archetype key at all, a name the world has no component of): the stage must go off
    // rather than skip every slice without a word.
    internal bool Accepts(SystemsNative.Slice* s, out string? off)
    {
        off = null;
        var archetype = Volatile.Read(ref _archetype);
        var when = Volatile.Read(ref _when);
        if (archetype is null && when is null) return true;
        nint layout = 0;
        var t = SystemsNative.T;
        var rc = SystemsNative.Unavailable;
        if (t != null)
        {
            nuint l = 0;
            rc = t->GameSliceArchetype(s, &l);
            if (rc == SystemsNative.Ok) layout = (nint)l;
        }
        if (archetype is not null)
        {
            if (rc == SystemsNative.NotFound) return false;  // an empty slice, an entity the map does not hold: this slice
            if (layout == 0 || t == null)
            {
                off = $"WhenArchetype cannot read a slice's archetype on this build (kiln.world answered {rc})";
                return false;
            }
            var key = (t->GameWorldSession((uint)World), layout);
            if (!archetype.Cache.TryGetValue(key, out var ok))
            {
                ok = true;
                for (var i = 0; i < archetype.Has.Length + archetype.Lacks.Length; i++)
                {
                    var has = i < archetype.Has.Length;
                    var r = t->GameArchetypeHas((uint)World, (nuint)layout, has ? archetype.Has[i] : archetype.Lacks[i - archetype.Has.Length]);
                    if (r < 0)
                    {
                        off = $"WhenArchetype: {(has ? archetype.HasNames[i] : archetype.LacksNames[i - archetype.Has.Length])} " +
                            $"is not a component of this world";
                        return false;
                    }
                    ok &= has ? r == 1 : r == 0;
                }
                archetype.Cache.TryAdd(key, ok);
            }
            if (!ok) return false;
        }
        return when is null || when(new SliceInfo(Program, World, (int)s->Count, s->Step, s->ThreadIsWorld != 0, layout));
    }

    internal void ForgetArchetypes()
    {
        var filter = Volatile.Read(ref _archetype);
        if (filter is not null)
            Interlocked.CompareExchange(ref _archetype, new ArchetypeFilter(filter.Has, filter.Lacks, filter.HasNames, filter.LacksNames), filter);
    }

    internal void OnDisabled(WrapDisabledEvent e)
    {
        try { Disabled?.Invoke(e); }
        catch (Exception x) { Mod?.Log.Error($"a WrapDisabled handler threw: {x.Message}"); }
    }

    internal static IReadOnlyList<WrapInfo> InfosOf(string program, WorldKind world) =>
        [.. Systems.Live.Keys.Where(s => s.Wrap.Program == program && s.Wrap.World == world)
            .Select(s => new WrapInfo(s.Wrap.ModId, program, world, s.Kind, s.Priority, s.State, s.PerEntity))];

    // The mod unloads: every stage of its owner goes, and the unload waits until no call of one runs (a stage that runs
    // finishes; none starts). Tick thread (ModContext.Retire).
    private void Retire()
    {
        lock (_lock) _disposed = true;
        var t = SystemsNative.T;
        if (t == null) return;
        uint pending = 0;
        t->GameWrapsRelease(Owner, &pending);
        var until = Environment.TickCount64 + 2000;
        while (pending != 0 && Environment.TickCount64 < until)
        {
            Thread.Yield();
            t->GameWrapsRelease(Owner, &pending);
        }
        if (pending != 0)
            Backend.Current.Log(0, LogLevel.Error, $"Kiln.Systems: {ModId}'s wrap stages still ran 2 s after its unload began");
        List<ManagedStage> stages;
        lock (_lock) stages = [.. _stages];
        foreach (var s in stages) s.Forget(pending == 0);
    }

    // ---- the tick: states, disabled stages, freed handles ----
    private static IDisposable? _tick;
    private static readonly System.Collections.Concurrent.ConcurrentQueue<Action> TickWork = new();

    internal static void PostToTick(Action a) => TickWork.Enqueue(a);

    internal static void EnsureTick()
    {
        if (_tick is not null) return;
        _tick = ServiceTick.Add("kiln.systems wraps", static (in TickInfo t) => Poll());
    }

    internal static void Poll()
    {
        while (TickWork.TryDequeue(out var a))
            try { a(); } catch (Exception e) { Backend.Current.Log(0, LogLevel.Error, $"Kiln.Systems: posted work threw: {e.Message}"); }
        var t = SystemsNative.T;
        foreach (var s in Systems.Live.Keys) s.Poll(t);
        WrapWrites.Flush();  // the wraps' writes of the step that ended: ComponentWrittenEvent, once per entity and component
    }
}

/// <summary>One stage of a wrap: a native kiln.world wrap whose user data is this object.</summary>
internal sealed unsafe class ManagedStage
{
    internal readonly ProgramWrap Wrap;
    internal readonly WrapStage Kind;
    internal readonly SliceHandler? SliceFn;
    internal readonly EntityHandler? Each;
    internal readonly EntityViewHandler? View;
    internal readonly int Priority;
    internal ulong Handle;
    internal volatile bool Removing;
    internal string? Detail;
    internal long ManagedFaults;
    internal IAround? Around;           // the stage is one half of an AroundEach
    internal ManagedStage? Partner;     // its other half: they go off together
    private GCHandle _self;
    private volatile int _disabled;   // 1: an exception turned it off here
    private volatile bool _raised;
    private WrapState _state = WrapState.Pending;

    [ThreadStatic] internal static bool OriginalCalled;
    [ThreadStatic] private static EntityHandler? _each;
    [ThreadStatic] private static EntityViewHandler? _view;
    [ThreadStatic] private static ViewContext? _ctx;
    [ThreadStatic] private static SystemsNative.Slice* _slice;
    [ThreadStatic] private static Exception? _eachError;

    internal ManagedStage(ProgramWrap wrap, WrapStage kind, SliceHandler? slice, Delegate? each, int priority)
    {
        Wrap = wrap;
        Kind = kind;
        SliceFn = slice;
        Each = each as EntityHandler;
        View = each as EntityViewHandler;
        Priority = priority;
    }

    internal bool PerEntity => Each is not null || View is not null;

    internal WrapState State => _state;

    internal void Refuse(string why)
    {
        Detail = why;
        _state = WrapState.Refused;
    }

    internal void Install(ulong owner)
    {
        var t = SystemsNative.T;
        if (t == null) throw new KilnUnavailableException("systems.wraps", "kiln.world v4 is not here");
        var index = Systems.FindIndex(t, Wrap.Program, Wrap.World);
        if (index < 0)
        {
            Refuse($"{Wrap.Program}: no such program in the {Systems.Lower(Wrap.World)} world");
            return;
        }
        _self = GCHandle.Alloc(this);
        ulong handle = 0;
        var rc = t->GameProgramWrap(owner, (uint)Wrap.World, index, (int)Kind, Priority, &OnStage,
            (void*)GCHandle.ToIntPtr(_self), &handle);
        if (rc != SystemsNative.Ok)
        {
            _self.Free();
            Refuse(rc switch
            {
                SystemsNative.Unavailable => "wraps are off (native SET loaded, or the client gate)",
                SystemsNative.NotFound => $"{Wrap.Program}: no such program now",
                _ => $"refused ({rc})",
            });
            return;
        }
        Handle = handle;
        var info = Info(t);
        if (info.State == SystemsNative.Refused)
        {
            Detail = NativeDetail(info);
            if (info.Reason == SystemsNative.ReasonReplaceTaken) Detail = $"{Wrap.Program} is replaced by another wrap ({Detail})";
            _state = WrapState.Refused;
            _self.Free();
            return;
        }
        Systems.Live.TryAdd(this, 0);
        ProgramWrap.EnsureTick();
    }

    internal SystemsNative.WrapInfo Info(SystemsNative.Table* t)
    {
        var i = new SystemsNative.WrapInfo { Size = (uint)sizeof(SystemsNative.WrapInfo) };
        if (t != null && Handle != 0) t->GameWrapInfo(Handle, &i);
        return i;
    }

    private static string NativeDetail(SystemsNative.WrapInfo i) => SystemsNative.Text(i.Detail, 128);

    internal void Unwrap()
    {
        Removing = true;
        var t = SystemsNative.T;
        if (t != null && Handle != 0 && t->GameProgramUnwrap(Handle) == SystemsNative.WrongThread)
            ProgramWrap.PostToTick(Unwrap);  // native forbids removal inside a slice; Removing already closes managed admission
    }

    internal void Forget(bool idle)
    {
        Removing = true;
        _state = WrapState.Removed;
        if (idle)
        {
            Systems.Live.TryRemove(this, out _);
            if (_self.IsAllocated) _self.Free();
        }
        // A timed-out unload retains the handle and stays polled until native removal and running == 0.
    }

    // Tick thread: the native state, a stage kiln.world turned off, a handle no call can use any more.
    internal void Poll(SystemsNative.Table* t)
    {
        if (t == null || Handle == 0) return;
        var i = Info(t);
        var state = i.State switch
        {
            SystemsNative.Active => WrapState.Active,
            SystemsNative.Removed => WrapState.Removed,
            SystemsNative.Refused => WrapState.Refused,
            _ => WrapState.Pending,
        };
        if (_state == WrapState.Active && state == WrapState.Pending) Wrap.ForgetArchetypes();  // a new world
        _state = state;
        if (!_raised && (i.Disabled != 0 || (state == WrapState.Removed && i.Reason == SystemsNative.ReasonNativeSet)))
        {
            var why = i.Disabled == SystemsNative.ReasonFault ? $"a fault (0x{i.FaultCode:x8})" :
                i.Disabled == SystemsNative.ReasonBudget ? "budget" : "native SET loaded";
            RaiseDisabled(why);
        }
        if ((state is WrapState.Removed or WrapState.Refused) && i.Running == 0)
        {
            Systems.Live.TryRemove(this, out _);
            if (_self.IsAllocated) _self.Free();
        }
    }

    internal void RaiseDisabled(string reason)
    {
        if (_raised) return;
        _raised = true;
        var e = new WrapDisabledEvent(Wrap.Program, Wrap.ModId, Kind, reason);
        if (Backend.Current.IsTickThread) Wrap.OnDisabled(e);
        else ProgramWrap.PostToTick(() => Wrap.OnDisabled(e));
    }

    // Turned off for a reason that is not the mod's (the build cannot do what the stage asks): no count for its policy.
    private void Off(string reason)
    {
        if (Interlocked.Exchange(ref _disabled, 1) != 0) return;
        Unwrap();
        RaiseDisabled(reason);
        Partner?.Off($"the other half of its AroundEach went off: {reason}");
    }

    private void Fail(Exception e)
    {
        Interlocked.Increment(ref ManagedFaults);
        if (Interlocked.Exchange(ref _disabled, 1) != 0) return;
        Unwrap();  // the stage is off from now on; kiln.world takes it out in the next step
        RaiseDisabled($"it threw {e.GetType().Name}: {e.Message}");
        Partner?.Off($"the other half of its AroundEach threw {e.GetType().Name}");
        // the host's mod policy counts it (5 in 60 s disable the mod): rethrown as scheduled work of the mod
        var info = ExceptionDispatchInfo.Capture(e);
        if (Wrap.Mod is { } mod) mod.Scheduler.Post(() => info.Throw());
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnStage(SystemsNative.Slice* s, void* user)
    {
        try
        {
            if (GCHandle.FromIntPtr((nint)user).Target is ManagedStage stage) stage.Run(s);
        }
        catch
        {
            // never unwind through the game
        }
    }

    private void Run(SystemsNative.Slice* s)
    {
        var replace = Kind == WrapStage.Replace;
        var mod = Wrap.Mod;
        var previous = ModContext.CurrentMod;
        SliceGuard.Depth++;
        OriginalCalled = false;
        try
        {
            ModContext.CurrentMod = mod;
            string? off = null;
            if (_disabled != 0 || Removing || mod is { Alive: false } || !Wrap.Accepts(s, out off))
            {
                if (off is not null) Off(off);       // the filter cannot decide here: the stage goes off and says why
                if (replace) RunOriginalQuietly(s);  // this slice is the game's
                return;
            }
            if (PerEntity) Iterate(s);
            else
            {
                var slice = new Slice(s, this);
                SliceFn!(ref slice);
            }
        }
        catch (Exception e)
        {
            Fail(e);
            if (replace && !OriginalCalled) RunOriginalQuietly(s);
        }
        finally
        {
            ModContext.CurrentMod = previous;
            SliceGuard.Depth--;
        }
    }

    private static void RunOriginalQuietly(SystemsNative.Slice* s)
    {
        var t = SystemsNative.T;
        if (t != null) t->GameSliceRunOriginal(s);
    }

    private void Iterate(SystemsNative.Slice* s)
    {
        var t = SystemsNative.T;
        if (t == null) return;
        var around = Around;
        if (around is not null && !around.Begin(Kind, s)) return;  // an After whose Before kept nothing of this slice
        var outerEach = _each;
        var outerView = _view;
        var outerCtx = _ctx;
        var outerSlice = _slice;
        _each = Each;
        _view = View;
        _ctx = View is null ? null : ViewContext.Begin(t, s, this);
        _slice = s;
        _eachError = null;
        var rc = SystemsNative.Ok;
        try
        {
            rc = t->GameSliceIterate(s, &OnEntity, null);
        }
        finally
        {
            _ctx?.End();  // the last entity's writes, even when a handler threw
            around?.End(Kind, _eachError is null && rc == SystemsNative.Ok);
            _each = outerEach;
            _view = outerView;
            _ctx = outerCtx;
            _slice = outerSlice;
        }
        if (_eachError is { } e)
        {
            _eachError = null;
            ExceptionDispatchInfo.Capture(e).Throw();
        }
        // kiln.world could not iterate the slice (UNAVAILABLE: the iterator or the job's counts did not resolve on this
        // build; BUFFER_TOO_SMALL: an iterator over 4096 bytes): the stage would see no entity, so it goes off and says why
        if (rc != SystemsNative.Ok)
            Off(rc == SystemsNative.BufferTooSmall
                ? $"per-entity stages cannot run on {Wrap.Program}: its iterator is larger than kiln.world's 4096 bytes"
                : $"per-entity stages cannot run on this build: kiln.world cannot iterate {Wrap.Program}'s slices (result {rc})");
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint OnEntity(ulong* iter, uint row, uint entity, void* user)
    {
        try
        {
            if (_view is { } view)
            {
                var e = new EntityView(_slice, iter, entity, (int)row, _ctx!);
                view(ref e);
            }
            else _each?.Invoke(new EntityId(entity));
            return 0;
        }
        catch (Exception e)
        {
            _eachError = e;
            return 1;
        }
    }
}
