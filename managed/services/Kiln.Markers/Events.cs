// SPDX-License-Identifier: MIT
// Markers' service events (chapter 04 §4.6): one native subscription for the process (a process-owned owner, not a mod's),
// whose callbacks copy each payload and raise the managed handlers, each as code of the mod that added it. A mod's handlers
// go when it unloads; a handler that throws is logged and removed. These are service events, not Kiln.Events posts.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>One accepted change of a marker's logical value (not of its visibility). Immutable; safe to read on any
/// thread.</summary>
[AnyThread]
public sealed class MarkerChangedEvent
{
    internal MarkerChangedEvent(string owner, string id, MarkerChangeKind kind, MarkerSource audience, EntityId recipient, ulong worldEpoch,
        ulong peerGeneration, ulong publisherGeneration, ulong sequence, int affected, string cause)
    {
        Owner = owner;
        Id = id;
        Kind = kind;
        Audience = audience;
        Recipient = recipient;
        WorldEpoch = worldEpoch;
        PeerGeneration = peerGeneration;
        PublisherGeneration = publisherGeneration;
        Sequence = sequence;
        Affected = affected;
        Cause = cause;
    }

    /// <summary>The publisher's namespace (a mod's id).</summary>
    public string Owner { get; }

    /// <summary>The marker's key; empty for <see cref="MarkerChangeKind.Cleared"/>.</summary>
    public string Id { get; }

    /// <summary>What happened.</summary>
    public MarkerChangeKind Kind { get; }

    /// <summary>Whose record: a local one, a server layer's, or a received one.</summary>
    public MarkerSource Audience { get; }

    /// <summary>The player a per-player server change is for; <see cref="EntityId.None"/> otherwise.</summary>
    public EntityId Recipient { get; }

    /// <summary>kiln.markers' epoch of the world the record belongs to.</summary>
    public ulong WorldEpoch { get; }

    /// <summary>The link generation of a received record; 0 otherwise.</summary>
    public ulong PeerGeneration { get; }

    /// <summary>The publisher's binding generation (a new one each time a publisher binds).</summary>
    public ulong PublisherGeneration { get; }

    /// <summary>The order of accepted changes (rising).</summary>
    public ulong Sequence { get; }

    /// <summary>Records changed: 1, or the count a <see cref="MarkerChangeKind.Cleared"/> stands for.</summary>
    public int Affected { get; }

    /// <summary>Why: <c>Api</c> (a call), <c>OwnerRetired</c> (its mod unloaded), <c>WorldStopped</c>, <c>PeerLeft</c>,
    /// <c>Snapshot</c> or <c>LinkStale</c>.</summary>
    public string Cause { get; }
}

public static unsafe partial class Markers
{
    /// <summary>Every accepted change of every publisher's markers, in accepted order, at the next step of their world
    /// (a client's own markers: the client world's step, on the game's main thread). When a world stops, its last changes
    /// arrive on the stopping thread before the stop completes: do not call game code from the handler then. A change a
    /// handler makes arrives with the next step. Identical values and empty clears raise nothing; a clear raises one
    /// <see cref="MarkerChangeKind.Cleared"/>. A handler that throws is logged and removed; a mod's handlers go when it
    /// unloads.</summary>
    [AnyThread]
    public static event Action<MarkerChangedEvent>? Changed
    {
        add
        {
            ChangedHandlers.Add(value);
            EnsureSubscribed();
        }
        remove => ChangedHandlers.Remove(value);
    }

    internal static readonly MarkerHandlers<Action<MarkerChangedEvent>> ChangedHandlers = new("Markers.Changed");

    // ---- the native subscription (one for the process) ----

    private static readonly object SubscribeLock = new();
    private static ulong _subscription;
    private static long _subscribedEpoch = -1;

    /// <summary>Subscribes to kiln.markers once per process (and again under a new registry binding: the test kit's next
    /// harness), with an owner of Kiln's own.</summary>
    internal static void EnsureSubscribed()
    {
        var t = MarkersNative.T;
        if (t == null) return;
        var epoch = Capabilities.CurrentBinding.Epoch;
        lock (SubscribeLock)
        {
            if (_subscription != 0 && _subscribedEpoch == epoch) return;
            var owner = Backend.Current.OwnerCreate("kiln.markers");
            if (owner == 0) return;  // no core in this process
            ulong h = 0;
            if (t->Subscribe(owner, &OnChange, null, null, null, &h) != MarkersNative.Ok) return;
            _subscription = h;
            _subscribedEpoch = epoch;
        }
    }

    /// <summary>Tests: forget the subscription (the fake table is gone).</summary>
    internal static void ForgetSubscription()
    {
        lock (SubscribeLock)
        {
            _subscription = 0;
            _subscribedEpoch = -1;
        }
    }

    private static readonly string[] Causes = ["Api", "OwnerRetired", "WorldStopped", "PeerLeft", "Snapshot", "LinkStale"];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnChange(MarkersNative.Change* c, void* user)
    {
        try
        {
            if (c == null || c->Size < (uint)sizeof(MarkersNative.Change)) return;
            var e = new MarkerChangedEvent(MarkersNative.Text(c->Owner, 65), MarkersNative.Text(c->Key, 65),
                c->Kind <= 3 ? (MarkerChangeKind)c->Kind : MarkerChangeKind.Updated,
                c->Audience <= 3 ? (MarkerSource)c->Audience : MarkerSource.Received, new EntityId(c->Recipient), c->WorldEpoch,
                c->PeerGeneration, c->PublisherGeneration, c->Sequence, (int)c->Affected,
                c->Cause < (uint)Causes.Length ? Causes[c->Cause] : $"cause {c->Cause}");
            ChangedHandlers.Raise(h => h(e));
        }
        catch (Exception x)
        {
            try { Backend.Current.Log(0, LogLevel.Error, $"kiln.markers: a change could not be delivered: {x.Message}"); }
            catch { /* nothing left to say */ }
        }
    }
}

/// <summary>Handlers owned by mods: run as the mod's code, removed when it unloads or when one throws (logged).</summary>
internal sealed class MarkerHandlers<T>(string name) where T : Delegate
{
    private readonly object _lock = new();
    private (T Handler, ModContext? Owner)[] _items = [];
    private readonly HashSet<ModContext> _watched = new(ReferenceEqualityComparer.Instance);

    public int Count
    {
        get { lock (_lock) return _items.Length; }
    }

    public void Add(T? handler)
    {
        if (handler is null) return;
        var owner = ModContext.CurrentMod;
        bool watch;
        lock (_lock)
        {
            _items = [.. _items, (handler, owner)];
            watch = owner is not null && _watched.Add(owner);
        }
        if (watch) owner!.Retiring += () => Drop(owner);
    }

    public void Remove(T? handler)
    {
        if (handler is null) return;
        lock (_lock)
        {
            var i = Array.FindLastIndex(_items, x => x.Handler == handler);
            if (i >= 0) _items = _items.Where((_, k) => k != i).ToArray();
        }
    }

    private void Drop(ModContext mod)
    {
        lock (_lock)
        {
            _items = _items.Where(i => !ReferenceEquals(i.Owner, mod)).ToArray();
            _watched.Remove(mod);
        }
    }

    public void Raise(Action<T> call)
    {
        (T, ModContext?)[] items;
        lock (_lock) items = _items;
        foreach (var (h, owner) in items)
        {
            if (owner is not null && !owner.Alive) continue;
            if (Invoke(owner, () => call(h))) continue;
            lock (_lock)
            {
                var i = Array.FindIndex(_items, x => ReferenceEquals(x.Handler, h) && ReferenceEquals(x.Owner, owner));
                if (i >= 0) _items = _items.Where((_, k) => k != i).ToArray();
            }
        }
    }

    // As kiln.network's NetModCalls.Invoke: the mod's synchronization context and current mod around the call.
    private bool Invoke(ModContext? mod, Action action)
    {
        var previousContext = SynchronizationContext.Current;
        var previousMod = ModContext.CurrentMod;
        if (mod is not null)
        {
            SynchronizationContext.SetSynchronizationContext(mod.Scheduler.SynchronizationContext);
            ModContext.CurrentMod = mod;
        }
        try
        {
            action();
            return true;
        }
        catch (Exception e)
        {
            var text = $"{name} handler threw and was removed: {e}";
            if (mod is not null) mod.Log.Error(e, $"{name} handler threw and was removed");
            else Backend.Current.Log(0, LogLevel.Error, "kiln.markers: " + text);
            return false;
        }
        finally
        {
            if (mod is not null)
            {
                ModContext.CurrentMod = previousMod;
                SynchronizationContext.SetSynchronizationContext(previousContext);
            }
        }
    }
}
