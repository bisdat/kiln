// SPDX-License-Identifier: MIT
// Typed program wraps (chapter 06a 5.3, 9.3, 9.6): Systems.Wrap<T> with a view Kiln.Game generates for each of the
// game's programs (Ecs.Programs.*, tools/typegen/typegen.py programs, from the committed program catalog), the contract
// check against the live descriptor, typed per-entity stages and AroundEach.
using System.Buffers;

namespace Kiln;

/// <summary>A typed view of one of the game's ECS programs: Kiln.Game generates one per program of the program catalog
/// (<c>Kiln.Ecs.Programs.FuelSetup</c> for <c>fuel_setup</c>), with a property for each component its descriptor lists
/// that the type dump has a layout for. Used through <see cref="Systems.Wrap{T}(WrapOptions?)"/>; a view is valid during
/// its per-entity call only.</summary>
/// <typeparam name="TSelf">The view itself.</typeparam>
public interface IGameProgram<TSelf> where TSelf : IGameProgram<TSelf>, allows ref struct
{
    /// <summary>The program's name (<c>"fuel_setup"</c>).</summary>
    static abstract string Name { get; }

    /// <summary>FNV-1a-32 of the program's contract text (its 21 access lists, three masks, flags and interval, chapter 06a
    /// 9.7), as the catalog the view was made from says. <see cref="Systems.Wrap{T}(WrapOptions?)"/> refuses a program whose
    /// live descriptor has another.</summary>
    static abstract uint Contract { get; }

    /// <summary>The contract text the view was made from, without its empty lists (one <c>list=names</c> line each, then
    /// the masks, flags and interval): what a refusal compares the live descriptor with.</summary>
    static abstract string Lists { get; }

    /// <summary>The view of one entity of a per-entity stage.</summary>
    /// <exception cref="InvalidOperationException"><paramref name="entity"/> is of another program, or that program's live
    /// descriptor does not have the view's contract.</exception>
    static abstract TSelf Bind(EntityView entity);
}

/// <summary>A typed per-entity stage: runs once per entity of each slice, on the thread that runs the slice.</summary>
public delegate void EntityHandler<T>(ref T entity) where T : allows ref struct;

/// <summary>What <see cref="ProgramWrap{T}.AroundEach{TSnap}"/> keeps of an entity before the game's update.</summary>
public delegate TSnap Capture<T, TSnap>(in T entity) where T : allows ref struct;

/// <summary>What <see cref="ProgramWrap{T}.AroundEach{TSnap}"/> does with an entity after the game's update, with what
/// its capture kept before it.</summary>
public delegate void Apply<T, TSnap>(ref T entity, in TSnap before) where T : allows ref struct;

/// <summary>A typed wrap (<see cref="Systems.Wrap{T}(WrapOptions?)"/>): the stages of <see cref="ProgramWrap"/> plus
/// per-entity stages that see the program's typed view <typeparamref name="T"/>, and <see cref="AroundEach{TSnap}"/>.
/// Dispose removes every stage.</summary>
public sealed class ProgramWrap<T> : IDisposable where T : IGameProgram<T>, allows ref struct
{
    internal ProgramWrap(ProgramWrap untyped) => Untyped = untyped;

    /// <summary>The untyped wrap under this one (the same stages).</summary>
    public ProgramWrap Untyped { get; }

    /// <summary>The program's name.</summary>
    public string Program => Untyped.Program;

    /// <summary>The wrap's world.</summary>
    public WorldKind World => Untyped.World;

    /// <summary>See <see cref="ProgramWrap.State"/>.</summary>
    [AnyThread]
    public WrapState State => Untyped.State;

    /// <summary>See <see cref="ProgramWrap.Stats"/>.</summary>
    [AnyThread]
    public WrapStats Stats => Untyped.Stats;

    /// <summary>See <see cref="ProgramWrap.Disabled"/>.</summary>
    public event Action<WrapDisabledEvent>? Disabled
    {
        add => Untyped.Disabled += value;
        remove => Untyped.Disabled -= value;
    }

    /// <summary>A slice stage before the game's update (see <see cref="ProgramWrap.Before"/>).</summary>
    [TickThreadOnly]
    public ProgramWrap<T> Before(SliceHandler handler, int? priority = null)
    {
        Untyped.Before(handler, priority);
        return this;
    }

    /// <summary>A slice stage after the game's update (see <see cref="ProgramWrap.After"/>).</summary>
    [TickThreadOnly]
    public ProgramWrap<T> After(SliceHandler handler, int? priority = null)
    {
        Untyped.After(handler, priority);
        return this;
    }

    /// <summary>Per entity before the game's update, with the entity's typed view: read what the program reads, write what
    /// it writes (marked for the save and replication; <see cref="ComponentWrittenEvent"/> in the next step).</summary>
    [TickThreadOnly]
    public ProgramWrap<T> BeforeEach(EntityHandler<T> handler, int? priority = null)
    {
        ArgumentNullException.ThrowIfNull(handler);
        Untyped.BeforeEach((ref EntityView e) =>
        {
            var view = T.Bind(e);
            handler(ref view);
        }, priority);
        return this;
    }

    /// <summary>Per entity after the game's update, with the entity's typed view.</summary>
    [TickThreadOnly]
    public ProgramWrap<T> AfterEach(EntityHandler<T> handler, int? priority = null)
    {
        ArgumentNullException.ThrowIfNull(handler);
        Untyped.AfterEach((ref EntityView e) =>
        {
            var view = T.Bind(e);
            handler(ref view);
        }, priority);
        return this;
    }

    /// <summary>Around the game's update, per entity: <paramref name="capture"/> runs for every entity of the slice before
    /// the update (one <typeparamref name="TSnap"/> per entity, kept for this slice only), the update runs, then
    /// <paramref name="apply"/> runs for every entity with what its capture kept: compensation for a program whose effect
    /// you scale or undo (chapter 06a 9.6). One Before and one After stage at <paramref name="priority"/>: an onion around
    /// the higher priorities' stages. A capture that throws turns both off; the slice's snapshots are dropped, the
    /// update runs as the game's.</summary>
    [TickThreadOnly]
    public ProgramWrap<T> AroundEach<TSnap>(Capture<T, TSnap> capture, Apply<T, TSnap> apply, int? priority = null) where TSnap : unmanaged
    {
        ArgumentNullException.ThrowIfNull(capture);
        ArgumentNullException.ThrowIfNull(apply);
        Untyped.Around(new Around<T, TSnap>(capture, apply), priority);
        return this;
    }

    /// <summary>Instead of the game's update (see <see cref="ProgramWrap.Replace"/>).</summary>
    [TickThreadOnly]
    public WrapResult<ProgramWrap<T>> Replace(SliceHandler handler)
    {
        var r = Untyped.Replace(handler);
        return new WrapResult<ProgramWrap<T>>(r.Ok, r.Reason, r.Ok ? this : null);
    }

    /// <summary>See <see cref="ProgramWrap.When"/>.</summary>
    public ProgramWrap<T> When(Func<SliceInfo, bool> filter)
    {
        Untyped.When(filter);
        return this;
    }

    /// <summary>See <see cref="ProgramWrap.WhenArchetype"/>.</summary>
    public ProgramWrap<T> WhenArchetype(IEnumerable<string> has, IEnumerable<string>? lacks = null)
    {
        Untyped.WhenArchetype(has, lacks);
        return this;
    }

    /// <summary>Removes every stage: no stage call starts after this returns.</summary>
    [TickThreadOnly]
    public void Dispose() => Untyped.Dispose();
}

/// <summary>The two stages of an AroundEach: what a slice needs before its iteration and after it.</summary>
internal unsafe interface IAround
{
    EntityViewHandler CaptureView { get; }
    EntityViewHandler ApplyView { get; }
    /// <summary>Before the stage's iteration of slice <paramref name="s"/>. False: nothing to do (an After without a
    /// capture of this slice on this thread).</summary>
    bool Begin(WrapStage kind, SystemsNative.Slice* s);
    /// <summary>After it (also when a handler threw: <paramref name="ok"/> false).</summary>
    void End(WrapStage kind, bool ok);
}

internal sealed unsafe class Around<T, TSnap> : IAround where T : IGameProgram<T>, allows ref struct where TSnap : unmanaged
{
    private const int MostWaiting = 64;  // captures whose After never came (a stage gone between), per thread

    // A frame names its AroundEach by number, not by reference: one whose After never came (the stages went between
    // them) must not keep the mod's delegates, or its load context, alive on a game worker thread.
    private sealed class Frame
    {
        public long Owner;
        public nint Ctx;
        public ulong Step;
        public TSnap[] Snaps = [];
        public uint[] Ids = [];
        public int Filled;
    }

    [ThreadStatic] private static List<Frame>? t_waiting;  // captured slices waiting for their After on this thread
    [ThreadStatic] private static Frame? t_now;             // the slice being captured or applied

    private static long _next;
    private readonly long _id = Interlocked.Increment(ref _next);
    private readonly Capture<T, TSnap> _capture;
    private readonly Apply<T, TSnap> _apply;

    internal Around(Capture<T, TSnap> capture, Apply<T, TSnap> apply)
    {
        _capture = capture;
        _apply = apply;
        CaptureView = OnCapture;
        ApplyView = OnApply;
    }

    public EntityViewHandler CaptureView { get; }
    public EntityViewHandler ApplyView { get; }

    public bool Begin(WrapStage kind, SystemsNative.Slice* s)
    {
        var waiting = t_waiting ??= new List<Frame>();
        if (kind == WrapStage.Before)
        {
            for (var i = waiting.Count - 1; i >= 0; i--)
                if (waiting[i].Owner == _id || waiting.Count > MostWaiting)
                {
                    Return(waiting[i]);
                    waiting.RemoveAt(i);
                }
            var n = (int)s->Count;
            t_now = new Frame
            {
                Owner = _id,
                Ctx = (nint)s->ProgramCtx,
                Step = s->Step,
                Snaps = n > 0 ? ArrayPool<TSnap>.Shared.Rent(n) : [],
                Ids = n > 0 ? ArrayPool<uint>.Shared.Rent(n) : [],
            };
            return true;
        }
        for (var i = waiting.Count - 1; i >= 0; i--)
        {
            var f = waiting[i];
            if (f.Owner != _id || f.Ctx != (nint)s->ProgramCtx || f.Step != s->Step) continue;
            waiting.RemoveAt(i);
            t_now = f;
            return true;
        }
        return false;
    }

    public void End(WrapStage kind, bool ok)
    {
        var f = t_now;
        t_now = null;
        if (f is null) return;
        if (kind == WrapStage.Before && ok) (t_waiting ??= new List<Frame>()).Add(f);
        else Return(f);
    }

    private static void Return(Frame f)
    {
        if (f.Snaps.Length > 0) ArrayPool<TSnap>.Shared.Return(f.Snaps);
        if (f.Ids.Length > 0) ArrayPool<uint>.Shared.Return(f.Ids);
        f.Snaps = [];
        f.Ids = [];
        f.Filled = 0;
    }

    private void OnCapture(ref EntityView e)
    {
        var f = t_now;
        var row = e.Row;
        if (f is null || (uint)row >= (uint)f.Snaps.Length) return;
        var view = T.Bind(e);
        f.Snaps[row] = _capture(in view);
        f.Ids[row] = e.Entity.Value;
        if (row >= f.Filled) f.Filled = row + 1;
    }

    private void OnApply(ref EntityView e)
    {
        var f = t_now;
        var row = e.Row;
        if (f is null || row >= f.Filled || f.Ids[row] != e.Entity.Value) return;  // rows do not move inside a slice
        var view = T.Bind(e);
        _apply(ref view, in f.Snaps[row]);
    }
}

/// <summary>What a typed wrap's refusal says: the lists (and masks, flags, interval) a live descriptor's contract text
/// has otherwise than the view's (chapter 06a 9.7's text, one <c>key=value</c> line each).</summary>
internal static class Contracts
{
    private static readonly HashSet<string> Scalars = ["sys_read_opt", "sys_write_opt", "sys_write_parallel", "flags", "interval"];

    internal static string Differences(string program, string expected, string live)
    {
        var want = Parse(expected);
        var have = Parse(live);
        var keys = have.Keys.Concat(want.Keys.Where(k => !have.ContainsKey(k))).ToList();
        var parts = new List<string>();
        foreach (var key in keys)
        {
            var a = want.GetValueOrDefault(key, "");
            var b = have.GetValueOrDefault(key, "");
            if (a == b) continue;
            if (Scalars.Contains(key))
            {
                parts.Add($"{key} changed: {(a.Length > 0 ? a : "none")} -> {(b.Length > 0 ? b : "none")}");
                continue;
            }
            var was = a.Length == 0 ? [] : a.Split(',');
            var now = b.Length == 0 ? [] : b.Split(',');
            var added = now.Except(was).Select(n => "+" + n);
            var removed = was.Except(now).Select(n => "-" + n);
            var change = string.Join(" ", added.Concat(removed));
            parts.Add($"{key} list changed: {(change.Length > 0 ? change : "its order")}");
        }
        return $"{program}: the live descriptor is not the view's (made for another game build): " +
            (parts.Count > 0 ? string.Join("; ", parts) : "its contract differs");
    }

    private static Dictionary<string, string> Parse(string text)
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var line in text.Split('\n'))
        {
            var eq = line.IndexOf('=');
            if (eq > 0) d[line[..eq]] = line[(eq + 1)..];
        }
        return d;
    }
}
