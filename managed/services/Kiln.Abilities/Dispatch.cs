// SPDX-License-Identifier: MIT
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Kiln.Abilities' native listener (Pre, tick thread) and job-thread wrap dispatcher. Lives in the default
/// load context: the function pointers handed to native stay valid for the life of the process; mods' delegates are
/// reached only through these tables and dropped when a mod retires.</summary>
internal static unsafe class AbilitiesDispatch
{
    private static int _installed;
    private static readonly HashSet<ModContext> WatchedMods = new(ReferenceEqualityComparer.Instance);
    private static readonly object Gate = new();

    /// <summary>Per-step work of the service (cast timers, sequence polling); run in the listener.</summary>
    internal static event Action<ulong>? Tick;

    internal static ulong Step;

    public static void Ensure()
    {
        if (Volatile.Read(ref _installed) == 1) return;
        AbilitiesCaps.Ensure();
        var t = AbilitiesNative.T;
        if (t == null) return;
        if (Interlocked.Exchange(ref _installed, 1) == 1) return;
        t->SetListener(&Listener, null);
        t->SetWrapDispatcher(&WrapDispatch, null);
        ModLog.Info("abilities: managed listener installed");
    }

    public static void Watch(ModContext mod)
    {
        lock (Gate)
            if (!WatchedMods.Add(mod)) return;
        mod.UnloadStarting += () => CloseAdmission(mod);  // before its OnUnload
        mod.Retiring += () => Retire(mod);                // after it
    }

    // The start of the mod's unload, before its OnUnload: no new call enters its sync natives or attachments; the first
    // time the unload waits the bounded RetireWaitMs for the calls inside, later asks only look. Null when none is
    // inside, else what the host's deferred OnUnload waits for.
    internal static string? CloseAdmission(ModContext mod)
    {
        var gate = GateOf(mod);
        var first = Interlocked.Exchange(ref gate.Closed, 1) == 0;
        if (first ? WaitQuiet(gate) : Volatile.Read(ref gate.Inflight) == 0) return null;
        return $"{Volatile.Read(ref gate.Inflight)} call(s) still inside its native handlers (kiln.abilities)";
    }

    private static void Retire(ModContext mod)
    {
        // review P1: admission first: from here no call enters this mod's sync natives or attachments
        var gate = GateOf(mod);
        Interlocked.Exchange(ref gate.Closed, 1);
        Abilities.Natives.Unregister(mod);
        RemoveWraps(mod);
        RemoveHoldFilters(mod);
        AbilityRegistry.Retire(mod);
        lock (Gate) WatchedMods.Remove(mod);
        if (AbilitiesNative.T is var t && t != null && mod.Owner != 0)
        {
            // the core takes an owner off on the tick thread outside a native call: else in the next tick (review P1)
            if (!Game.IsTickThread || t->ReleaseOwner(mod.Owner) == AbilitiesNative.WrongThread) PendingRelease.Enqueue(mod.Owner);
        }
        RetireGate(mod, gate);
    }

    // ----------------------------------------------------------------------------------------------- mod gates ----
    // review P1: every call into a mod's sync native or attachment enters the mod's gate (ModGate.Inflight) before its
    // handler runs; the mod's retirement closes the gate first, then waits a bounded 2 ms for the calls inside. One still
    // inside after that (a handler that blocks, or a retirement from inside a handler on this thread) is reported and
    // kept: its entry objects stay reachable from the running call, and the retirement finishes in a later tick, when
    // the gate is quiet. The gate closes at the start of the mod's unload (ModContext.UnloadStarting), before its
    // OnUnload, with the same bounded wait: OnUnload runs with no call inside unless one outlasts the bound.

    internal sealed class ModGate
    {
        public int Inflight;
        public int Closed;
        public string Id = "";
    }

    private static readonly ConditionalWeakTable<ModContext, ModGate> Gates = new();
    private static readonly ModGate NoMod = new() { Id = "(no mod)" };
    private static readonly List<(ModGate Gate, ulong Since)> Deferred = new();
    [ThreadStatic] private static int _inside;  // dispatch depth on this thread

    internal static int RetireWaitMs = 2;  // the bound; tests raise it so a busy machine cannot decide their outcome

    internal static ModGate GateOf(ModContext? mod) => mod is null ? NoMod : Gates.GetValue(mod, m => new ModGate { Id = m.Info.Id });

    // A call enters (true) or the mod is retiring (false). The increment comes before the check (both full fences).
    private static bool Enter(ModGate g)
    {
        Interlocked.Increment(ref g.Inflight);
        if (Volatile.Read(ref g.Closed) == 0) return true;
        Interlocked.Decrement(ref g.Inflight);
        return false;
    }

    // True when no call is inside, waiting at most RetireWaitMs; never waits on this thread's own call.
    internal static bool WaitQuiet(ModGate g)
    {
        if (Volatile.Read(ref g.Inflight) == 0) return true;
        if (_inside > 0) return false;  // a retirement from inside a handler: its own call is inside
        var sw = Stopwatch.StartNew();
        while (Volatile.Read(ref g.Inflight) != 0 && sw.ElapsedMilliseconds < RetireWaitMs) Thread.Yield();
        return Volatile.Read(ref g.Inflight) == 0;
    }

    internal static void RetireGate(ModContext mod, ModGate gate)
    {
        if (WaitQuiet(gate)) return;
        lock (Gate) Deferred.Add((gate, Step));
        mod.Log.Warn($"abilities: {gate.Id} unloads with {Volatile.Read(ref gate.Inflight)} call(s) still inside its native handlers: they take no new call; " +
                     "what they use stays until they leave (checked each tick)");
    }

    // The listener's tick: deferred retirements whose calls left.
    internal static void FinishDeferred()
    {
        lock (Gate)
        {
            for (var i = Deferred.Count - 1; i >= 0; i--)
            {
                var (g, since) = Deferred[i];
                if (Volatile.Read(ref g.Inflight) != 0) continue;
                Deferred.RemoveAt(i);
                ModLog.Info($"abilities: the last native handler call of {g.Id} left {Step - since} tick(s) after its unload: retired");
            }
        }
    }

    internal static int DeferredCount { get { lock (Gate) return Deferred.Count; } }

    // ----------------------------------------------------------------------------------------------- interest -----

    private static uint Interest()
    {
        uint m = 0;
        if (Events.HasSubscribers<ImpactStartedEvent>()) m |= AbilitiesNative.Bit(AbilitiesNative.EvImpactStarted);
        if (Events.HasSubscribers<ImpactStartingEvent>()) m |= AbilitiesNative.InterestHold;
        if (Events.HasSubscribers<ImpactEndedEvent>()) m |= AbilitiesNative.Bit(AbilitiesNative.EvImpactEnded);
        if (Events.HasSubscribers<ImpactNativeEvent>()) m |= AbilitiesNative.Bit(AbilitiesNative.EvNative);
        if (Events.HasSubscribers<ImpactSignalEvent>()) m |= AbilitiesNative.Bit(AbilitiesNative.EvSignal);
        if (!Abilities.Natives.ManagedNatives.IsEmpty) m |= AbilitiesNative.Bit(AbilitiesNative.EvManagedCall);
        return m;
    }

    // ----------------------------------------------------------------------------------------------- listener -----

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint Listener(AbilitiesNative.Event* evs, uint n, void* user)
    {
        try
        {
            if (n > 0) Deliver(evs, (int)n);
        }
        catch (Exception e)
        {
            ModLog.Error($"abilities: listener threw: {e}");
        }
        try
        {
            DrainDetaches();
            FinishDeferred();
            CheckSetupRefusals();
        }
        catch (Exception e)
        {
            ModLog.Error($"abilities: detaching threw: {e}");
        }
        try
        {
            Step++;
            Tick?.Invoke(Step);
        }
        catch (Exception e)
        {
            ModLog.Error($"abilities: service tick threw: {e}");
        }
        try
        {
            return Interest();
        }
        catch
        {
            return 0;
        }
    }

    private static void Deliver(AbilitiesNative.Event* evs, int n)
    {
        for (var i = 0; i < n; i++)
        {
            ref var ev = ref evs[i];
            var impact = new ImpactRef(ev.ImpactId, ev.ProgramId, new EntityId(ev.Self), new EntityId(ev.Root));
            switch (ev.Kind)
            {
                case AbilitiesNative.EvImpactStarted:
                    if (ev.Held != 0 && Events.HasSubscribers<ImpactStartingEvent>())
                    {
                        var se = Events.Post(new ImpactStartingEvent(impact, ev.Step));
                        if (se.Canceled) ev.Canceled = 1;
                    }
                    if (Events.HasSubscribers<ImpactStartedEvent>()) Events.Post(new ImpactStartedEvent(impact, ev.Step));
                    break;
                case AbilitiesNative.EvImpactEnded:
                    if (Events.HasSubscribers<ImpactEndedEvent>()) Events.Post(new ImpactEndedEvent(impact, ev.Step));
                    break;
                case AbilitiesNative.EvNative:
                    if (!Events.HasSubscribers<ImpactNativeEvent>()) break;
                    var snap = Snapshot(ref ev, impact);
                    Events.Post<ImpactNativeEvent>(ev.NativeHash switch
                    {
                        0xb04d0e7a => new ImpactDamageEvent(snap),
                        0x91f30b26 => new ImpactBuffEvent(snap),
                        0x61b11b03 => new ImpactSpawnEvent(snap),
                        _ => new ImpactNativeEvent(snap),
                    });
                    break;
                case AbilitiesNative.EvSignal:
                    if (!Events.HasSubscribers<ImpactSignalEvent>()) break;
                    var val = new byte[Math.Min(ev.ValueSize, 64)];
                    for (var k = 0; k < val.Length; k++) val[k] = ev.Value[k];
                    Events.Post(new ImpactSignalEvent(impact, ev.Tag, ev.Site, ev.ValueType, val, ev.Step));
                    break;
                case AbilitiesNative.EvManagedCall:
                    Answer(ref ev, impact);
                    break;
            }
        }
    }

    private static NativeSnapshot Snapshot(ref AbilitiesNative.Event ev, ImpactRef impact)
    {
        var argc = (int)Math.Min(ev.Argc, 31);
        var args = new uint[argc];
        var values = new uint[argc];
        var offs = new byte[argc];
        var sizes = new byte[argc];
        for (var k = 0; k < argc; k++)
        {
            args[k] = ev.Args[k];
            values[k] = ev.ArgValues[k];
            offs[k] = ev.ArgOffsets[k];
            sizes[k] = ev.ArgSizes[k];
        }
        var payload = new byte[136];
        for (var k = 0; k < payload.Length; k++) payload[k] = ev.Value[k];
        return new NativeSnapshot
        {
            Hash = ev.NativeHash, Impact = impact, Site = ev.Site, Step = ev.Step, Result = ev.Result, Error = ev.Error,
            Args = args, Values = values, Offsets = offs, Sizes = sizes, Payload = payload,
        };
    }

    private static void Answer(ref AbilitiesNative.Event ev, ImpactRef impact)
    {
        var t = AbilitiesNative.T;
        if (!Abilities.Natives.ManagedNatives.TryGetValue(ev.NativeHash, out var m) || m.Mod is { Alive: false })
        {
            t->NativeComplete(ev.CallId, 0, 0);
            return;
        }
        var args = new uint[Math.Min(ev.Argc, 31)];
        for (var k = 0; k < args.Length; k++) args[k] = ev.Args[k];
        var call = new NativeInvocation(m.Name, impact, ev.Site, args);
        byte value = 0, error = 0;
        var outerMod = ModContext.CurrentMod;
        var outerSync = SynchronizationContext.Current;
        try
        {
            ModContext.CurrentMod = m.Mod;
            if (m.Mod is not null) SynchronizationContext.SetSynchronizationContext(m.Mod.Scheduler.SynchronizationContext);
            value = m.Handler(call);
        }
        catch (Exception e)
        {
            error = 0x11;  // keen::ErrorId Internal: the instance stops with Error
            (m.Mod?.Log ?? null)?.Error(e, $"native {m.Name} threw");
        }
        finally
        {
            ModContext.CurrentMod = outerMod;
            SynchronizationContext.SetSynchronizationContext(outerSync);
        }
        t->NativeComplete(ev.CallId, error, value);
    }

    // ----------------------------------------------------------------------------------------------- wraps (job) ---

    internal sealed class WrapEntry
    {
        public required uint Hash;
        public required NativeWrapHandler Handler;
        public required int Priority;
        public required ModContext? Mod;
        // NativeCall.Bytes for this handler: the gate's answer for Mod, decided at registration (where the mod is known)
        // and read by every call of the handler on the job threads.
        public required bool RawAllowed;
        public long Order;
        public int Faults;
        public bool Active = true;
    }

    private sealed class WrapHandle(WrapEntry e) : IDisposable
    {
        public void Dispose() => RemoveWrap(e);
    }

    // hash -> handlers, sorted (copy on write; job threads read without locks)
    private static readonly ConcurrentDictionary<uint, WrapEntry[]> Wraps = new();
    private static long _order;
    private static readonly StrongBox<long> InFlight = new(0);

    internal static IDisposable AddWrap(uint hash, NativeWrapHandler handler, int priority)
    {
        Game.AssertTickThread();
        Ensure();
        var t = AbilitiesNative.Require("Abilities.Natives.Wrap");
        var mod = ModContext.CurrentMod;
        // since 0.2.0-dev.29: a wrap is a Before attachment that takes every call, at its priority (the same order as
        // the global dispatcher's); an older kiln.abilities keeps the global dispatcher
        if (AbilitiesNative.V2(nameof(AbilitiesNative.TableV2.NativeDetach)) != null)
        {
            var r = Attach(hash, 0, 0, NativeStage.Before, priority, handler, mod, wrap: true);
            if (!r.Ok) throw new InvalidOperationException($"Natives.Wrap(0x{hash:x8}): {r.Reason} (not a vanilla native?)");
            if (mod is not null) Watch(mod);
            mod?.Log.Info($"abilities: wraps native {Abilities.Natives.NameOf(hash)} (job thread)");
            return r.Handle!;
        }
        var e = Insert(hash, handler, priority, mod);
        var rc = t->NativeWrap(hash, 1);
        if (rc != AbilitiesNative.Ok)
        {
            RemoveWrap(e);
            throw new InvalidOperationException($"Natives.Wrap(0x{hash:x8}): {AbilitiesNative.Describe(rc)} (not a vanilla native?)");
        }
        if (mod is not null) Watch(mod);
        mod?.Log.Info($"abilities: wraps native {Abilities.Natives.NameOf(hash)} (job thread)");
        return new WrapHandle(e);
    }

    // Puts the handler in the job-thread table (no native call: AddWrap asks the core to route the native here). The
    // registering mod is known here, so NativeCall.Bytes' answer for it is decided here (tests call this directly).
    internal static WrapEntry Insert(uint hash, NativeWrapHandler handler, int priority, ModContext? mod)
    {
        var e = new WrapEntry
        {
            Hash = hash, Handler = handler, Priority = priority, Mod = mod, RawAllowed = mod is { Info.Advanced: true },
            Order = Interlocked.Increment(ref _order),
        };
        lock (Gate)
        {
            var cur = Wraps.TryGetValue(hash, out var a) ? a : [];
            Wraps[hash] = cur.Append(e).OrderByDescending(x => x.Priority).ThenBy(x => x.Order).ToArray();
        }
        return e;
    }

    internal static void RemoveWrap(WrapEntry e)
    {
        bool empty;
        lock (Gate)
        {
            e.Active = false;
            if (!Wraps.TryGetValue(e.Hash, out var a)) return;
            var next = a.Where(x => !ReferenceEquals(x, e)).ToArray();
            empty = next.Length == 0;
            if (empty) Wraps.TryRemove(e.Hash, out _);
            else Wraps[e.Hash] = next;
        }
        if (empty && AbilitiesNative.T is var t && t != null) t->NativeWrap(e.Hash, 0);
        Drain();
    }

    private static void RemoveWraps(ModContext mod)
    {
        foreach (var kv in Wraps)
            foreach (var e in kv.Value)
                if (ReferenceEquals(e.Mod, mod)) RemoveWrap(e);
        foreach (var kv in Attaches)
            if (ReferenceEquals(kv.Value.Mod, mod)) Detach(kv.Value);
    }

    // ----------------------------------------------------------------------------------------------- attachments --
    // since 0.2.0-dev.29: one native attachment per handler (Natives.Attach, and Natives.Wrap / OnDamage as Before
    // attachments on every call). The core gets a static dispatcher and an id (no GCHandle): a detached id answers
    // nothing (the call goes on as if the handler were not there), and nothing has to be freed after the core let go.

    internal sealed class AttachEntry
    {
        public required long Id;
        public required uint Hash;
        public required NativeStage Stage;
        public required NativeWrapHandler Handler;
        public required ModContext? Mod;
        public required bool RawAllowed;
        public required bool Wrap;  // Natives.Wrap / OnDamage: its log line names a wrap
        public required ModGate ModGate;
        public ulong Native;        // the core's handle
        public int Faults;
        public volatile bool Active = true;
    }


    private static readonly ConcurrentDictionary<long, AttachEntry> Attaches = new();
    private static readonly ConcurrentQueue<ulong> PendingDetach = new();
    private static readonly ConcurrentQueue<ulong> PendingRelease = new();
    private static long _attachIds;

    internal static delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void> AttachDispatcher => &AttachDispatch;

    // tick thread
    internal static AttachResult Attach(uint hash, uint program, uint site, NativeStage stage, int priority,
        NativeWrapHandler handler, ModContext? mod, bool wrap = false)
    {
        var t = AbilitiesNative.V2(nameof(AbilitiesNative.TableV2.NativeDetach));
        if (t == null) throw new KilnUnavailableException(AbilitiesCaps.NativesAttach, "kiln.abilities is older than 0.2.0-dev.29");
        Ensure();
        var e = InsertAttach(hash, stage, handler, mod, wrap);
        var filter = new AbilitiesNative.NativeFilter { Size = (uint)sizeof(AbilitiesNative.NativeFilter), ProgramId = program, Site = site };
        ulong handle = 0;
        uint refusal = 0;
        var rc = t->NativeAttach(mod?.Owner ?? 0, hash, &filter, (int)stage, priority, AttachDispatcher, (void*)e.Id, &handle, &refusal);
        if (rc != AbilitiesNative.Ok)
        {
            e.Active = false;
            Attaches.TryRemove(e.Id, out _);
            if (rc == AbilitiesNative.InvalidArgument && refusal != AbilitiesNative.RefusedNone)
                return new AttachResult(false, AbilitiesNative.Refusal(refusal), null);
            if (rc == AbilitiesNative.Unavailable) throw new KilnUnavailableException(AbilitiesCaps.NativesAttach, "kiln.abilities has no natives on this build");
            if (rc == AbilitiesNative.WrongThread) throw new InvalidOperationException("Natives.Attach: not on the tick thread (or inside a native call)");
            throw new ArgumentException($"Natives.Attach(0x{hash:x8}): {AbilitiesNative.Describe(rc)}");
        }
        e.Native = handle;
        if (mod is not null) Watch(mod);
        if (!wrap)
            mod?.Log.Info($"abilities: {stage} attached to native {Abilities.Natives.NameOf(hash)} (program 0x{program:x8}, site 0x{site:x8}, priority {priority})");
        return new AttachResult(true, null, new NativeAttachment(e));
    }

    // Puts the handler in the table the dispatcher reads (the core is asked by Attach; tests call this directly). The
    // registering mod is known here, so NativeCall.Bytes' answer for it is decided here.
    internal static AttachEntry InsertAttach(uint hash, NativeStage stage, NativeWrapHandler handler, ModContext? mod, bool wrap = false)
    {
        var e = new AttachEntry
        {
            Id = Interlocked.Increment(ref _attachIds), Hash = hash, Stage = stage, Handler = handler, Mod = mod,
            RawAllowed = mod is { Info.Advanced: true }, Wrap = wrap, ModGate = GateOf(mod),
        };
        Attaches[e.Id] = e;
        return e;
    }

    // Off at once (a call that comes now runs no handler); the core lets go on the tick thread outside a native call,
    // else in the next Pre.
    internal static void Detach(AttachEntry e)
    {
        e.Active = false;
        if (!Attaches.TryRemove(e.Id, out _) || e.Native == 0) return;
        var t = AbilitiesNative.V2(nameof(AbilitiesNative.TableV2.NativeDetach));
        if (t == null) return;
        if (!Game.IsTickThread || t->NativeDetach(e.Native) == AbilitiesNative.WrongThread) PendingDetach.Enqueue(e.Native);
    }

    private static void DrainDetaches()
    {
        if (!PendingRelease.IsEmpty && AbilitiesNative.T is var rt && rt != null)
            while (PendingRelease.TryDequeue(out var owner))
                if (rt->ReleaseOwner(owner) == AbilitiesNative.WrongThread) { PendingRelease.Enqueue(owner); break; }
        if (PendingDetach.IsEmpty) return;
        var t = AbilitiesNative.V2(nameof(AbilitiesNative.TableV2.NativeDetach));
        while (PendingDetach.TryDequeue(out var h))
            if (t != null) t->NativeDetach(h);
    }

    internal static int AttachCount(ModContext? mod) => Attaches.Values.Count(e => ReferenceEquals(e.Mod, mod));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void AttachDispatch(AbilitiesNative.NativeCall* c, void* user)
    {
        Interlocked.Increment(ref InFlight.Value);
        var outer = ModContext.CurrentMod;
        try
        {
            if (!Attaches.TryGetValue((long)user, out var e) || !e.Active || e.Mod is { Alive: false }) return;  // as if absent
            if (!Enter(e.ModGate)) return;  // its mod is retiring
            _inside++;
            ModContext.CurrentMod = e.Mod;
            try
            {
                var call = new NativeCall(c, e.Mod, e.RawAllowed);
                e.Handler(ref call);
            }
            catch (Exception ex)
            {
                if (Interlocked.Increment(ref e.Faults) <= 3)
                    e.Mod?.Log.Error($"abilities: {(e.Wrap ? "wrap" : e.Stage + " attachment")} of {Abilities.Natives.CachedName(e.Hash)} threw: {ex.GetType().Name}: {ex.Message}");
            }
            finally
            {
                _inside--;
                Interlocked.Decrement(ref e.ModGate.Inflight);
            }
        }
        catch
        {
            // never throw into native code
        }
        finally
        {
            ModContext.CurrentMod = outer;
            Interlocked.Decrement(ref InFlight.Value);
        }
    }

    // Waits for job threads still inside a handler (they cannot enter removed ones any more).
    private static void Drain()
    {
        var sw = Stopwatch.StartNew();
        while (Interlocked.Read(ref InFlight.Value) > 0 && sw.ElapsedMilliseconds < 1000) Thread.SpinWait(64);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void WrapDispatch(AbilitiesNative.NativeCall* c, void* user)
    {
        Interlocked.Increment(ref InFlight.Value);
        var outer = ModContext.CurrentMod;
        try
        {
            if (!Wraps.TryGetValue(c->Hash, out var handlers)) return;  // OutError stays 0xFF: the game's native runs
            foreach (var h in handlers)
            {
                if (!h.Active || h.Mod is { Alive: false }) continue;
                ModContext.CurrentMod = h.Mod;
                try
                {
                    var call = new NativeCall(c, h.Mod, h.RawAllowed);
                    h.Handler(ref call);
                }
                catch (Exception e)
                {
                    if (Interlocked.Increment(ref h.Faults) <= 3)
                        h.Mod?.Log.Error($"abilities: wrap of {Abilities.Natives.NameOf(h.Hash)} threw: {e.GetType().Name}: {e.Message}");
                }
            }
        }
        catch
        {
            // never throw into native code
        }
        finally
        {
            ModContext.CurrentMod = outer;
            Interlocked.Decrement(ref InFlight.Value);
        }
    }

    // ----------------------------------------------------------------------------------------------- sync natives -
    // A synchronous native (RegisterSync) hands the core one static dispatcher and an id, never a GCHandle: the entry
    // lives in this table, a removed id answers 0x1F, and nothing has to be freed after the core let go of it.

    internal sealed class SyncEntry
    {
        public required long Id;
        public required string Name;
        public required NativeSyncHandler Handler;
        public required ModContext? Mod;
        public required bool RawAllowed;
        public required ModGate ModGate;
        public uint Hash;
        public int Faults;
        public bool Watched;  // registered before a world had its registry: its state is checked each tick until it is in
        public volatile bool Active = true;
    }

    private static readonly ConcurrentDictionary<long, SyncEntry> Syncs = new();
    private static long _syncIds;

    internal static delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void> SyncDispatcher => &SyncDispatch;

    internal static SyncEntry AddSync(string name, NativeSyncHandler handler, ModContext? mod)
    {
        var e = new SyncEntry
        {
            Id = Interlocked.Increment(ref _syncIds), Name = name, Handler = handler, Mod = mod,
            RawAllowed = mod is { Info.Advanced: true }, ModGate = GateOf(mod),
        };
        Syncs[e.Id] = e;
        return e;
    }

    internal static void RemoveSync(SyncEntry e)
    {
        e.Active = false;
        Syncs.TryRemove(e.Id, out _);
    }

    // The mod's sync natives (mod unload): off here first (a call that comes now answers 0x1F), then in the core, which
    // marks the slot dead on the tick thread outside the step (no call can be inside the handler then).
    internal static void RemoveSyncs(ModContext mod)
    {
        foreach (var kv in Syncs)
        {
            if (!ReferenceEquals(kv.Value.Mod, mod)) continue;
            RemoveSync(kv.Value);
            if (kv.Value.Hash != 0 && AbilitiesNative.T is var t && t != null && Game.IsTickThread) t->NativeUnregister(kv.Value.Hash);
        }
    }

    internal static int SyncCount(ModContext? mod) => Syncs.Values.Count(e => ReferenceEquals(e.Mod, mod));

    // review P1 item 4: a sync native registered before a world existed is checked each tick until the world's registry
    // took it; one the world's setup refused for room is reported to its mod (NativeRegistrationRefusedEvent, its log).
    private static void CheckSetupRefusals()
    {
        foreach (var e in Syncs.Values)
        {
            if (!e.Watched || !e.Active || e.Hash == 0) continue;
            switch (Abilities.Natives.StateOf(e.Hash))
            {
                case NativeState.Live:
                case NativeState.None:
                case NativeState.Dead:
                    e.Watched = false;
                    break;
                case NativeState.Refused:
                    e.Watched = false;
                    e.Mod?.Log.Warn($"abilities: sync native {e.Name} was accepted before the world and the world's native registry had no room for it: programs that call it stop with Error");
                    if (Events.HasSubscribers<NativeRegistrationRefusedEvent>())
                        Events.Post(new NativeRegistrationRefusedEvent(e.Name, e.Hash, AbilitiesNative.Refusal(AbilitiesNative.RefusedRegistryFull)));
                    break;
            }
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void SyncDispatch(AbilitiesNative.NativeCall* c, void* user)
    {
        Interlocked.Increment(ref InFlight.Value);
        var outer = ModContext.CurrentMod;
        try
        {
            if (!Syncs.TryGetValue((long)user, out var e) || !e.Active || e.Mod is { Alive: false })
            {
                c->OutError = 0x1F;  // taken off: the instance stops with Error, as for an unknown native
                return;
            }
            if (!Enter(e.ModGate))
            {
                c->OutError = 0x1F;  // its mod is retiring: as a removed native
                return;
            }
            _inside++;
            ModContext.CurrentMod = e.Mod;
            try
            {
                var call = new NativeCall(c, e.Mod, e.RawAllowed, sync: true);
                var value = e.Handler(ref call);
                c->OutError = 0;
                c->OutValue = value;
            }
            catch (Exception ex)
            {
                c->OutError = 0x11;  // keen::ErrorId Internal: the instance stops with Error, the game goes on
                c->OutValue = 0;
                if (Interlocked.Increment(ref e.Faults) <= 3)
                    e.Mod?.Log.Error($"abilities: sync native {e.Name} threw: {ex.GetType().Name}: {ex.Message}");
            }
            finally
            {
                _inside--;
                Interlocked.Decrement(ref e.ModGate.Inflight);
            }
        }
        catch
        {
            c->OutError = 0x11;  // never throw into native code
        }
        finally
        {
            ModContext.CurrentMod = outer;
            Interlocked.Decrement(ref InFlight.Value);
        }
    }

    // ----------------------------------------------------------------------------------------------- hold filter --

    private sealed class HoldFilter(ModContext? mod, uint[] ids) : IDisposable
    {
        public readonly ModContext? Mod = mod;
        public readonly uint[] Ids = ids;
        public void Dispose() => RemoveHold(this);
    }

    private static readonly List<HoldFilter> Holds = new();

    internal static IDisposable AddHoldFilter(uint[] ids)
    {
        ArgumentNullException.ThrowIfNull(ids);
        var f = new HoldFilter(ModContext.CurrentMod, ids.ToArray());
        lock (Gate) Holds.Add(f);
        if (f.Mod is not null) Watch(f.Mod);
        PushHolds();
        return f;
    }

    private static void RemoveHold(HoldFilter f)
    {
        lock (Gate) Holds.Remove(f);
        PushHolds();
    }

    private static void RemoveHoldFilters(ModContext mod)
    {
        lock (Gate) Holds.RemoveAll(h => ReferenceEquals(h.Mod, mod));
        PushHolds();
    }

    private static void PushHolds()
    {
        var t = AbilitiesNative.T;
        if (t == null) return;
        uint[] all;
        lock (Gate) all = Holds.SelectMany(h => h.Ids).Distinct().ToArray();
        fixed (uint* p = all) t->HoldFilter(p, (uint)all.Length);
    }
}
