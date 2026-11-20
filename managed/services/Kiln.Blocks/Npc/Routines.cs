// SPDX-License-Identifier: MIT
// Routines: the managed face of kiln.routines (native/core/abi/services/kiln_routines.h). A mod binds a day plan it
// defined (a RoutineBlock) to a villager; the service takes the plan's task at each of the villager's decisions, and
// the game's own code walks, sits, finds the bed and plays the animations. docs/kiln/services/routines.md.
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

/// <summary>
/// Villager routines: bind a day plan (<see cref="RoutineBlock"/>, defined with <c>Blocks.Define</c> or in the
/// <c>routines</c> section of blocks/*.json) to a villager. One routine per villager: the first mod that assigns one
/// holds the villager until it removes the routine or unloads.
/// <code>
/// var day = Blocks.Define("day", new RoutineBlock { Windows = [new(360, 1200, new InspectTask()), new(1320, 360, new SleepTask())], Fallback = new IdleTask() });
/// var result = await Routines.Assign(villager, day);
/// </code>
/// <para>The answer says the assignment changed, not that a task ran: it is <c>Ok</c> also when the villager is paused,
/// not in the world, or its first task cannot start. What tasks do comes as <see cref="RoutineTaskChangedEvent"/>.</para>
/// <para>Capability <c>routines.decisions</c>. The calls throw <see cref="KilnUnavailableException"/> naming it when it is
/// off (kiln.routines is not loaded, or the game build is not known);
/// decided from what is true at the call, not from a row read earlier.</para>
/// </summary>
[ServerOnly]
public static unsafe class Routines
{
    private const string Capability = BlockCapabilities.RoutineDecisions;

    /// <summary>Binds <paramref name="plan"/> to <paramref name="npc"/>, or replaces the calling mod's plan on it. The
    /// change is made before the next sim step and the task completes after that step.</summary>
    /// <returns><c>Ok</c> with the assignment's revision; or <c>Ok = false</c> with <c>OwnedBy:&lt;mod&gt;</c> (another
    /// mod's routine is on the villager), <c>WorldStopped</c> (no world runs, or it stopped before the change was made),
    /// <c>Revoked</c>, <c>QueueFull</c> (256 changes already wait for the next step) or <c>WorldLoading</c> (the world's
    /// saved routines are not bound again yet: its data or a mod's data files are not there; try again at a later step).</returns>
    /// <exception cref="ArgumentException">A plan another mod defined, or one that is no longer defined (its mod was
    /// reloaded: define it again and use the new handle).</exception>
    /// <exception cref="KilnUnavailableException"><c>routines.decisions</c> is off.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or not from mod code.</exception>
    [TickThreadOnly, RequiresCapability(Capability)]
    public static Task<RoutineResult> Assign(Villager npc, BlockHandle<RoutineBlock> plan)
    {
        ArgumentNullException.ThrowIfNull(npc);
        ArgumentNullException.ThrowIfNull(plan);
        var t = Table("Routines.Assign");
        var mod = Mod("Routines.Assign");
        if (!string.Equals(plan.Mod, mod.Info.Id, StringComparison.Ordinal))
            throw new ArgumentException($"the routine {plan} is a plan of mod {plan.Mod}: a mod assigns the plans it defined itself", nameof(plan));
        var def = RoutineRegistry.Find(plan)
                  ?? throw new ArgumentException($"the routine {plan} is not defined now (was its mod reloaded? Define it again and use the new handle)", nameof(plan));
        if (Gate(t, mod, npc.NpcId) is { } wait) return wait;
        var wire = Encoding.UTF8.GetBytes(def.Plan.Wire);
        fixed (byte* p = wire)
        {
            var pending = Begin(mod, npc.NpcId, out var id);
            ulong ticket = 0;
            return Finish(t->Assign(mod.Owner, npc.NpcId, new NativeStr { Ptr = p, Len = (nuint)wire.Length }, &Done, (void*)id, &ticket), id, pending, "Routines.Assign");
        }
    }

    /// <summary>Takes the calling mod's routine off <paramref name="npc"/>: the game chooses for it again.</summary>
    /// <returns><c>Ok</c> (also when the villager had no routine: then the revision is 0 and no event follows); or
    /// <c>Ok = false</c> with <c>OwnedBy:&lt;mod&gt;</c>, <c>WorldStopped</c>, <c>Revoked</c>, <c>QueueFull</c> or <c>WorldLoading</c>.</returns>
    /// <exception cref="KilnUnavailableException"><c>routines.decisions</c> is off.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or not from mod code.</exception>
    [TickThreadOnly, RequiresCapability(Capability)]
    public static Task<RoutineResult> Remove(Villager npc)
    {
        ArgumentNullException.ThrowIfNull(npc);
        var t = Table("Routines.Remove");
        var mod = Mod("Routines.Remove");
        if (Gate(t, mod, npc.NpcId) is { } wait) return wait;
        // a saved routine the restart refused (VersionMismatch) is not in kiln.routines: the saved row goes here
        if (RoutineSaves.RemoveMismatch(npc.NpcId, mod.Info.Id) is var removed and not 0)
            return System.Threading.Tasks.Task.FromResult(new RoutineResult(true, null, npc.NpcId, removed));
        var pending = Begin(mod, npc.NpcId, out var id);
        ulong ticket = 0;
        return Finish(t->Remove(mod.Owner, npc.NpcId, &Done, (void*)id, &ticket), id, pending, "Routines.Remove");
    }

    /// <summary>Ends the suspension a manual command put on <paramref name="npc"/>'s routine: it decides again at the
    /// villager's next decision, without a teleport. A <see cref="Villager.Follow"/> on it ends with the resume.</summary>
    /// <returns><c>Ok</c> (also when it was not suspended: then nothing changed and no event follows); or
    /// <c>Ok = false</c> with <c>NotAssigned</c>, <c>OwnedBy:&lt;mod&gt;</c>, <c>WorldStopped</c>, <c>Revoked</c>, <c>QueueFull</c> or <c>WorldLoading</c>.</returns>
    /// <exception cref="KilnUnavailableException"><c>routines.decisions</c> is off.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or not from mod code.</exception>
    [TickThreadOnly, RequiresCapability(Capability)]
    public static Task<RoutineResult> Resume(Villager npc)
    {
        ArgumentNullException.ThrowIfNull(npc);
        var t = Table("Routines.Resume");
        var mod = Mod("Routines.Resume");
        if (Gate(t, mod, npc.NpcId) is { } wait) return wait;
        // The calling mod's routine: a Follow on the villager ends now, before the Resume is queued, so no GoTo of it is
        // sent after the Resume (one sent before is dropped by kiln.routines at the Resume's step).
        if (Inspect(npc) is { } s && s.Owner == mod.Info.Id) npc.StopFollowing();
        var pending = Begin(mod, npc.NpcId, out var id);
        ulong ticket = 0;
        return Finish(t->Resume(mod.Owner, npc.NpcId, &Done, (void*)id, &ticket), id, pending, "Routines.Resume");
    }

    /// <summary>The routine on <paramref name="npc"/> now (any mod's), or null when it has none.</summary>
    /// <exception cref="KilnUnavailableException"><c>routines.decisions</c> is off.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    [TickThreadOnly, RequiresCapability(Capability)]
    public static RoutineSnapshot? Inspect(Villager npc)
    {
        ArgumentNullException.ThrowIfNull(npc);
        var t = Table("Routines.Inspect");
        var buffer = stackalloc byte[1024];
        nuint length = 0;
        var rc = t->Inspect(npc.NpcId, buffer, 1024, &length);
        if (rc == RoutineNative.NotFound) return null;
        if (rc == RoutineNative.BufferTooSmall)
        {
            var big = new byte[(int)length];
            fixed (byte* p = big)
            {
                rc = t->Inspect(npc.NpcId, p, length, &length);
                if (rc == RoutineNative.NotFound) return null;
                Check(rc, "Routines.Inspect");
                return Read(new ReadOnlySpan<byte>(p, (int)length));
            }
        }
        Check(rc, "Routines.Inspect");
        return Read(new ReadOnlySpan<byte>(buffer, (int)length));
    }

    private static RoutineSnapshot Read(ReadOnlySpan<byte> json)
    {
        var reader = new System.Text.Json.Utf8JsonReader(json);
        using var doc = System.Text.Json.JsonDocument.ParseValue(ref reader);
        return RoutineNative.Snapshot(doc.RootElement);
    }

    // ---- what is true now -------------------------------------------------------------------------------------------

    /// <summary>The table, when routines work at this moment: kiln.routines is here and its row reads available now (the
    /// symbols resolved, no hook failed). Else the exception that names the capability.</summary>
    private static RoutineNative.Table* Table(string api)
    {
        var t = RoutineNative.T;
        var on = Capabilities.Is(Capability);
        // The table is here and the row reads off: Kiln.Blocks may have lowered the row while kiln.routines was missing,
        // and the refresh that takes that back runs every 25 steps. Run it now and read again (BOARD 2026-11-13 21:21).
        if (t != null && !on && Backend.Current.IsTickThread && ServiceCaps.LookAgain()) on = Capabilities.Is(Capability);
        if (!BlockCapabilities.RoutinesOn(t != null, on))
            throw ServiceCaps.Unavailable(Capability, t == null ? "kiln.routines is not loaded" : null);
        Services.RequireTickThread(api);
        return t;
    }

    // The restart's saved routines first (a change before them would land under the saved revisions or be undone by
    // them), and the owner the calling mod's calls come with named to kiln.routines. A refusal to wait, or null.
    private static Task<RoutineResult>? Gate(RoutineNative.Table* t, ModContext mod, uint npcId)
    {
        if (RoutineSaves.Ensure() is { } wait) return System.Threading.Tasks.Task.FromResult(new RoutineResult(false, wait, npcId, 0));
        RoutineSaves.Claim(t, mod.Owner, mod.Info.Id);
        return null;
    }

    private static ModContext Mod(string api)
    {
        var mod = ModContext.CurrentMod ?? throw new InvalidOperationException($"{api} must be called from mod code");
        RoutineRuntime.Start();
        return mod;
    }

    private static void Check(int rc, string api)
    {
        switch (rc)
        {
            case RoutineNative.Ok: return;
            case RoutineNative.WrongThread: throw new InvalidOperationException($"{api} must be called on the tick thread (use Scheduler.Post)");
            case RoutineNative.Unavailable: throw ServiceCaps.Unavailable(Capability, $"{api}: kiln.routines answered unavailable");
            case RoutineNative.InvalidArgument: throw new ArgumentException($"{api}: kiln.routines refused an argument (kiln.log says which)");
            default: throw new InvalidOperationException($"{api}: kiln.routines answered {rc}");
        }
    }

    // ---- one task per ticket ----------------------------------------------------------------------------------------

    private sealed class Pending(ModContext mod, uint npcId)
    {
        public readonly ModContext Mod = mod;
        public readonly uint NpcId = npcId;
        public readonly TaskCompletionSource<RoutineResult> Source = new();
    }

    private static readonly ConcurrentDictionary<nint, Pending> Waiting = new();
    private static long _serial;

    // A place for the answer, before the call: the completion can only come after the call returned (on a later Post),
    // but the id must be known to the service when it does.
    private static Pending Begin(ModContext mod, uint npcId, out nint id)
    {
        id = (nint)Interlocked.Increment(ref _serial);
        var pending = new Pending(mod, npcId);
        Waiting[id] = pending;
        return pending;
    }

    private static Task<RoutineResult> Finish(int rc, nint id, Pending pending, string api)
    {
        var npcId = pending.NpcId;
        if (rc == RoutineNative.Ok)
        {
            RoutineRuntime.Watch(pending.Mod);
            return pending.Source.Task;
        }
        // refused at once: no ticket, and the callback is never called
        Waiting.TryRemove(id, out _);
        switch (rc)
        {
            case RoutineNative.BufferTooSmall:
                return System.Threading.Tasks.Task.FromResult(new RoutineResult(false, "QueueFull", npcId, 0));
            case RoutineNative.Unavailable:
                // The capability read available a moment ago. When it still does, the service has no world to change
                // (a refusal); when it does not, it went off in between (a hook that did not install).
                if (!Capabilities.Is(Capability)) throw ServiceCaps.Unavailable(Capability, $"{api}: kiln.routines answered unavailable");
                return System.Threading.Tasks.Task.FromResult(new RoutineResult(false, "WorldStopped", npcId, 0));
            default:
                Check(rc, api);
                throw new InvalidOperationException($"{api}: kiln.routines answered {rc}");
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Done(RoutineNative.Result* r, void* user)
    {
        try
        {
            if (r == null || !Waiting.TryRemove((nint)user, out var pending)) return;
            var ok = r->Ok != 0;
            pending.Source.TrySetResult(new RoutineResult(ok, ok ? null : r->Reason.ToString() is { Length: > 0 } why ? why : "Refused", r->NpcId, r->Revision));
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Error, $"routines: a completion was lost: {e.Message}");
        }
    }

    /// <summary>The mod unloads: its routines go with it (the service removes them before the next step), and the tasks
    /// it still waits for are canceled, so its continuations do not run on its unload.</summary>
    internal static void Retire(ModContext mod)
    {
        var t = RoutineNative.T;
        if (t != null && mod.Owner != 0) t->RevokeOwner(mod.Owner);
        foreach (var (id, pending) in Waiting)
            if (ReferenceEquals(pending.Mod, mod) && Waiting.TryRemove(id, out _))
                pending.Source.TrySetCanceled();
    }

    /// <summary>For the test kit's next harness.</summary>
    internal static void ResetForTests()
    {
        foreach (var (id, pending) in Waiting)
            if (Waiting.TryRemove(id, out _))
                pending.Source.TrySetCanceled();
        RoutineNative.Forget();
        RoutineRuntime.Forget();
        RoutineSaves.Forget();
    }
}

/// <summary>What Kiln.Blocks keeps running for routines: the subscription to the service's events, and which mods' unload
/// it watches.</summary>
internal static unsafe class RoutineRuntime
{
    private static readonly object Gate = new();
    private static readonly HashSet<ModContext> Watched = [];
    private static ulong _owner, _subscription;
    private static RoutineNative.Table* _subscribedTo;

    // the registry binding under which the data tick found no table: it does not ask again under it (a missing service
    // is looked for again by the capability refresh, which calls LookAgain when it appears)
    private static long _missed = -1;

    static RoutineRuntime() => ServiceCaps.WhenFound(RoutineNative.Service, LookAgain);

    private static void LookAgain() => Interlocked.Exchange(ref _missed, -1);

    /// <summary>Subscribes to the service's events, once per table (tick thread). Called by the routine calls and by
    /// Kiln.Blocks' data tick (<paramref name="fromTick"/>), so a mod that only listens gets its events too.</summary>
    public static void Start(bool fromTick = false)
    {
        if (!Backend.Current.IsTickThread) return;
        long epoch = 0;
        if (fromTick)
        {
            try { epoch = Capabilities.CurrentBinding.Epoch; }
            catch (Exception e) when (e is InvalidOperationException or NotSupportedException) { return; }
            if (Interlocked.Read(ref _missed) == epoch) return;
        }
        var t = RoutineNative.T;
        if (t == null)
        {
            if (fromTick) Interlocked.Exchange(ref _missed, epoch);
            return;
        }
        if (t == _subscribedTo) return;
        if (_owner == 0) _owner = Backend.Current.OwnerCreate("kiln.blocks.routines");
        ulong handle = 0;
        if (t->SubscribeEvents(_owner != 0 ? _owner : 1, &OnEvent, null, &handle) != RoutineNative.Ok) return;
        _subscribedTo = t;
        _subscription = handle;
    }

    public static void Watch(ModContext mod)
    {
        lock (Gate)
            if (!Watched.Add(mod)) return;
        mod.Retiring += () =>
        {
            lock (Gate) Watched.Remove(mod);
            Routines.Retire(mod);
        };
    }

    public static void Forget()
    {
        _subscribedTo = null;
        _subscription = 0;
        LookAgain();
        lock (Gate) Watched.Clear();
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnEvent(NativeStr json, void* user)
    {
        try
        {
            // on Post, on the tick thread; a world that stops off the tick thread: at the start of the next step
            var e = RoutineNative.Event(json.ToString());
            if (e is RoutineAssignmentChangedEvent a) RoutineSaves.Observe(a);
            if (Backend.Current.IsTickThread) Events.Post(e);
            else Events.Enqueue(e);
        }
        catch (Exception e)
        {
            // a handler's exception is the bus's to report; this is an event Kiln.Blocks could not read
            Backend.Current.Log(0, LogLevel.Error, $"routines: an event was not posted: {e.Message}");
        }
    }
}
