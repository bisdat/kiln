// SPDX-License-Identifier: MIT
// Named component watches (kiln.component_changes v1, native/core/abi/services/kiln_component_changes.h; chapter 07
// §4.6, slice S7): a mod names entities (or every player in the game) and components (Health, Stamina, Mana, Fuel,
// Inventory, SlotSelection), and is told once per step of every net change, with who wrote it where Kiln knows.
//
// The native service compares the watched pairs at the step's window calls (no game hook of its own); this file is
// its one consumer: once a step, in Pre, it reads the frozen batch of change records and posts the events, and it
// follows the watches' states. Nothing here is called from native code: there is no callback, no GCHandle, nothing a
// game thread could run into. While no watch exists, nothing runs at all.
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

/// <summary>The components a <see cref="ComponentWatchRequest"/> can name (a set). Since 0.2.0-dev.29.</summary>
[Flags]
public enum ObservedComponents
{
    /// <summary>None (not a valid request).</summary>
    None = 0,
    /// <summary>keen::ecs::Health (an attribute: current, maximum ...).</summary>
    Health = 1,
    /// <summary>keen::ecs::Stamina.</summary>
    Stamina = 2,
    /// <summary>keen::ecs::Mana.</summary>
    Mana = 4,
    /// <summary>keen::ecs::Fuel (torches, lamps: units, 64 per charge).</summary>
    Fuel = 8,
    /// <summary>keen::ecs::Inventory (an 8-slot inventory's item, count and item-entity triples).</summary>
    Inventory = 16,
    /// <summary>keen::ecs::SlotSelection.</summary>
    SlotSelection = 32,
    /// <summary>All six.</summary>
    All = Health | Stamina | Mana | Fuel | Inventory | SlotSelection,
}

/// <summary>What a watch follows. Since 0.2.0-dev.29.</summary>
public enum ComponentWatchScope
{
    /// <summary>The listed entities (1 to 128), as the lives they are when the watch is made.</summary>
    Entities = 0,
    /// <summary>Every player in the game, as they join and leave (the list must be empty).</summary>
    Players = 1,
}

/// <summary>Whether a watch reaches past its roots. Since 0.2.0-dev.29.</summary>
[Flags]
public enum ComponentWatchExpansion
{
    /// <summary>The roots' own components only.</summary>
    None = 0,
    /// <summary>Also Inventory on every 8-slot inventory linked from a root's InventorySetup (nested links followed: depth
    /// 3, at most 32 inventories and 64 setups) and Fuel on the item entities in them, followed again every step.
    /// Health, Stamina, Mana and SlotSelection stay on the root.</summary>
    PlayerInventoryAndItems = 1,
}

/// <summary>A watch's state (<see cref="ComponentWatch.State"/>, <see cref="ComponentWatchStateChangedEvent"/>). Since
/// 0.2.0-dev.29.</summary>
public enum ComponentWatchState
{
    /// <summary>Watching: every net change of its pairs is reported.</summary>
    Ready = 0,
    /// <summary>Stopped whole because it no longer fits a budget (4096 pairs, 128 roots a pair) or its changes were not read
    /// in time. Nothing of it is reported until <see cref="ComponentChanges.Resume"/>.</summary>
    SuspendedBudget = 1,
    /// <summary>Stopped whole because a root cannot be followed (gone, its id now another entity, an inventory tree Kiln
    /// cannot walk whole). Nothing of it is reported until <see cref="ComponentChanges.Resume"/>.</summary>
    SuspendedMetadata = 2,
    /// <summary>Ended: disposed, its mod unloaded, or its server world stopped. It cannot be resumed.</summary>
    Disposed = 3,
}

/// <summary>Who changed a component (<see cref="ComponentValueChangedEvent.Origin"/>). Since 0.2.0-dev.29.</summary>
public enum ChangeOrigin
{
    /// <summary>Kiln cannot say: no Kiln writer reported the change. It may be the game, another loader's mod, or a raw
    /// write. A change without a report is never called the game's.</summary>
    Unknown = 0,
    /// <summary>Reserved: not reported in this version.</summary>
    Game = 1,
    /// <summary>One source (a mod's id, or a Kiln service such as <c>kiln.combat</c>) reported writes whose chain leads
    /// from the bytes before to the bytes after. This is net attribution: it does not prove nothing else wrote in between
    /// and put the same bytes back.</summary>
    Kiln = 2,
    /// <summary>Kiln writers reported writes, but more than one source did, or their chain does not lead to the bytes
    /// after (something else wrote too), or reports were lost or too many to keep.</summary>
    Mixed = 3,
}

/// <summary>An attribute whose typed values <see cref="AttributeValueChangedEvent"/> carries. Since 0.2.0-dev.29.</summary>
public enum ObservedAttribute
{
    /// <summary>Health: current and maximum.</summary>
    Health = 1,
    /// <summary>Stamina: current and maximum.</summary>
    Stamina = 2,
    /// <summary>Mana: current and maximum.</summary>
    Mana = 3,
    /// <summary>Fuel: current and maximum, in the game's units (64 per charge).</summary>
    Fuel = 4,
}

/// <summary>What to watch (<see cref="ComponentChanges.Watch"/>). Since 0.2.0-dev.29.</summary>
/// <param name="Scope">The listed entities, or every player in the game.</param>
/// <param name="Entities">The roots for <see cref="ComponentWatchScope.Entities"/>: 1 to 128 distinct entities; empty for
/// <see cref="ComponentWatchScope.Players"/>. An entity filter of an event handler is not a watch: name them here.</param>
/// <param name="Components">A nonempty set of the six.</param>
/// <param name="Expansion">Whether linked inventories and their item entities are watched too.</param>
public sealed record ComponentWatchRequest(ComponentWatchScope Scope, IReadOnlyList<EntityId> Entities, ObservedComponents Components,
    ComponentWatchExpansion Expansion = ComponentWatchExpansion.None);

/// <summary>The answer of <see cref="ComponentChanges.Watch"/> and <see cref="ComponentChanges.Resume"/>. Since 0.2.0-dev.29.</summary>
/// <param name="Ok">True: the watch is ready.</param>
/// <param name="Reason">Why not (a budget, a root that cannot be followed, no server world); null when <paramref name="Ok"/>.</param>
/// <param name="Watch">The watch (for Watch: null when it was not made; for Resume: the same object).</param>
/// <param name="Pairs">The present (entity, component) pairs it covers, or would have needed.</param>
public sealed record ComponentWatchResult(bool Ok, string? Reason, ComponentWatch? Watch, int Pairs) : IKilnResult;

/// <summary>One watch (<see cref="ComponentChanges.Watch"/>). Dispose it to stop; it ends by itself when its mod unloads or
/// its server world stops. Since 0.2.0-dev.29.</summary>
public sealed class ComponentWatch : IDisposable
{
    internal ComponentWatch(ulong id, ulong owner, ModContext? mod, ComponentWatchRequest request, int pairs)
    {
        Id = id;
        Owner = owner;
        Mod = mod;
        Request = request;
        _pairs = pairs;
    }

    internal ulong Owner { get; }
    internal ModContext? Mod { get; }
    private volatile int _state;
    private volatile string? _reason;
    private long _lastComplete;
    private volatile int _pairs;

    /// <summary>The service's id of the watch (on <see cref="ComponentValueChangedEvent.WatchIds"/>).</summary>
    public ulong Id { get; }

    /// <summary>The world it watches: always <see cref="WorldKind.Server"/> in this version.</summary>
    public WorldKind World => WorldKind.Server;

    /// <summary>What it was made for.</summary>
    public ComponentWatchRequest Request { get; }

    /// <summary>Its state as of this step's Pre (a change raises <see cref="ComponentWatchStateChangedEvent"/>).</summary>
    [AnyThread]
    public ComponentWatchState State => (ComponentWatchState)_state;

    /// <summary>Why it is suspended or ended; null while ready.</summary>
    [AnyThread]
    public string? Reason => _reason;

    /// <summary>The last step whose changes it reported completely.</summary>
    [AnyThread]
    public ulong LastCompleteStep => (ulong)Interlocked.Read(ref _lastComplete);

    /// <summary>The present (entity, component) pairs it covered at its last refresh.</summary>
    [AnyThread]
    public int Pairs => _pairs;

    internal bool Update(uint state, string? reason, ulong lastComplete, int pairs)
    {
        Interlocked.Exchange(ref _lastComplete, (long)lastComplete);
        _pairs = pairs;
        var changed = _state != (int)state || !string.Equals(_reason, reason, StringComparison.Ordinal);
        _state = (int)state;
        _reason = reason;
        return changed;
    }

    /// <summary>Stops the watch: no change of it is reported from the next step on. Any thread; again is a no-op.</summary>
    [AnyThread]
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _state, (int)ComponentWatchState.Disposed) == (int)ComponentWatchState.Disposed) return;
        _reason ??= "disposed";
        ComponentChanges.Forget(this, remove: true);
    }
}

/// <summary>A watched component changed (<see cref="ComponentChanges.Watch"/>): its bytes before and after the step's
/// interval (from one compare to the next: the simulation and the Post and Pre around it), posted in the next Pre.
/// A net change: a value that changed and came back inside one interval raises nothing. One event per entity and
/// component per compare interval, whatever number of watches cover it. Adding an overlapping watch can split a step
/// into two intervals with different baselines and watch ids. Handlers only ever see pairs some watch covers; subscribing alone
/// watches nothing. Since 0.2.0-dev.29.</summary>
[ServerOnly, TickThreadOnly]
public sealed class ComponentValueChangedEvent : KilnEvent, IEntityEvent
{
    static ComponentValueChangedEvent() => ComponentChanges.Ensure();

    /// <summary>Makes the event (Kiln posts it; a mod or a test may make one to test its handler).</summary>
    public ComponentValueChangedEvent(EventEntityKey key, string component, byte[] before, byte[] after, ulong step, ChangeOrigin origin,
        string? sourceMod, IEnumerable<ulong> tokens, IEnumerable<EntityId> roots, IEnumerable<ulong> watches)
    {
        ArgumentNullException.ThrowIfNull(component);
        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(after);
        Key = key;
        Component = component;
        Before = before.ToArray();
        After = after.ToArray();
        Step = step;
        Origin = origin;
        SourceMod = sourceMod;
        OperationTokens = Array.AsReadOnly(tokens.ToArray());
        RootEntities = Array.AsReadOnly(roots.ToArray());
        WatchIds = Array.AsReadOnly(watches.ToArray());
    }

    /// <summary>Which life of the entity: a root's is kiln.decisions' key (its spawn time or player session); an entity
    /// reached through a root's links has no spawn time and a serial of its own.</summary>
    public EventEntityKey Key { get; }

    /// <summary>The entity whose component changed.</summary>
    public EntityId Entity => new(Key.Entity);

    /// <summary>The world: always <see cref="WorldKind.Server"/> in this version.</summary>
    public WorldKind World => WorldKind.Server;

    /// <summary>The component's short name: Health, Stamina, Mana, Fuel, Inventory or SlotSelection.</summary>
    public string Component { get; }

    /// <summary>The bytes before (a copy); empty when the component was absent or larger than 256 bytes.</summary>
    public ReadOnlyMemory<byte> Before { get; }

    /// <summary>The bytes after (a copy); empty when the component is absent now or larger than 256 bytes.</summary>
    public ReadOnlyMemory<byte> After { get; }

    /// <summary>The step whose compare saw it.</summary>
    public ulong Step { get; }

    /// <summary>Who changed it, as far as Kiln knows (<see cref="ChangeOrigin"/>).</summary>
    public ChangeOrigin Origin { get; }

    /// <summary>The reporting sources (a mod's id, or a Kiln service such as <c>kiln.combat</c>), several joined by
    /// <c>", "</c>; the native list may be truncated to 127 bytes; null when none reported.</summary>
    public string? SourceMod { get; }

    /// <summary>The operation tokens of the writes Kiln's writers reported in the interval (at most 32).</summary>
    public IReadOnlyList<ulong> OperationTokens { get; }

    /// <summary>The watched roots this entity belongs to (itself, or the players whose inventory holds it).</summary>
    public IReadOnlyList<EntityId> RootEntities { get; }

    /// <summary>The watches that cover the pair.</summary>
    public IReadOnlyList<ulong> WatchIds { get; }

    /// <summary>True when the component was there before.</summary>
    public bool WasPresent => !Before.IsEmpty;

    /// <summary>True when the component is there now.</summary>
    public bool IsPresent => !After.IsEmpty;

    /// <inheritdoc/>
    public bool Involves(EntityId entity) => !entity.IsNone && (entity == Entity || RootEntities.Contains(entity));
}

/// <summary>The typed face of a <see cref="ComponentValueChangedEvent"/> of Health, Stamina, Mana or Fuel whose current or
/// maximum value changed: the values as the game's attribute code reads them, read at the compares (no live read when
/// it is posted). Not raised where Kiln cannot read the typed values on this build (kiln.combat's attributes for Health,
/// Stamina and Mana, kiln.items' fuel for Fuel); the raw event is raised either way. Since 0.2.0-dev.29.</summary>
[ServerOnly, TickThreadOnly]
public sealed class AttributeValueChangedEvent(ComponentValueChangedEvent change, ObservedAttribute attribute, int before, int after,
    int maximumBefore, int maximumAfter) : KilnEvent, IEntityEvent
{
    static AttributeValueChangedEvent() => ComponentChanges.Ensure();

    /// <summary>The raw change it is about.</summary>
    public ComponentValueChangedEvent Change { get; } = change;

    /// <summary>Which attribute.</summary>
    public ObservedAttribute Attribute { get; } = attribute;

    /// <summary>The current value before (the game's signed integer; Fuel in units).</summary>
    public int Before { get; } = before;

    /// <summary>The current value after.</summary>
    public int After { get; } = after;

    /// <summary>The maximum before.</summary>
    public int MaximumBefore { get; } = maximumBefore;

    /// <summary>The maximum after.</summary>
    public int MaximumAfter { get; } = maximumAfter;

    /// <inheritdoc/>
    public bool Involves(EntityId entity) => Change.Involves(entity);
}

/// <summary>A watch was suspended or ended by Kiln (a budget, a root that cannot be followed, a queue that was not read,
/// the world's end, its mod's unload). Posted in the Pre after it happened. Not raised for the mod's own
/// <see cref="ComponentWatch.Dispose"/> or <see cref="ComponentChanges.Resume"/>, whose results say it. Since 0.2.0-dev.29.</summary>
[ServerOnly, TickThreadOnly]
public sealed class ComponentWatchStateChangedEvent(ulong watchId, ComponentWatchState state, string? reason, ulong lastCompleteStep) : KilnEvent
{
    static ComponentWatchStateChangedEvent() => ComponentChanges.Ensure();

    /// <summary>The watch.</summary>
    public ulong WatchId { get; } = watchId;

    /// <summary>Its new state.</summary>
    public ComponentWatchState State { get; } = state;

    /// <summary>Why.</summary>
    public string? Reason { get; } = reason;

    /// <summary>The last step whose changes it reported completely.</summary>
    public ulong LastCompleteStep { get; } = lastCompleteStep;
}

/// <summary>Named component watches. Since 0.2.0-dev.29.</summary>
public static unsafe class ComponentChanges
{
    /// <summary>The capability row: <c>events.component_net</c>.</summary>
    public const string Capability = "events.component_net";

    /// <summary>Starts a watch. The answer is <see cref="ComponentWatchResult.Ok"/> false, with the reason and no watch, when
    /// it would exceed a budget (32 watches; 4096 present (entity, component) pairs over all watches; 128 roots for any
    /// pair), when a root cannot be followed (it does not exist, or it has no identity Kiln can tell from a later entity
    /// of its id: no spawn time and not a player in the game), or when no server world runs (a client that joined a game).
    /// The first changes are reported from the step after the next one (a baseline is read at the next step's start).
    /// Pre or Post on the tick thread.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="request"/> is null.</exception>
    /// <exception cref="ArgumentException">An empty, too long (over 128) or repeating list for
    /// <see cref="ComponentWatchScope.Entities"/>, a list for <see cref="ComponentWatchScope.Players"/>, an entity that is
    /// none, no component or one outside the six, an unknown expansion.</exception>
    /// <exception cref="KilnUnavailableException"><c>events.component_net</c> is off.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or inside a world program's update.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Capability)]
    public static ComponentWatchResult Watch(ComponentWatchRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        var roots = Validate(request);
        Services.RequireTickThread("ComponentChanges.Watch");
        var t = Use();
        var mod = ModContext.CurrentMod;
        var owner = mod?.Owner is { } o && o != 0 ? o : _owner;
        if (owner == 0) throw ServiceCaps.Unavailable(Capability, "no owner could be made for the watch");
        var rq = new ComponentChangesNative.WatchRequest
        {
            Size = (uint)sizeof(ComponentChangesNative.WatchRequest),
            Scope = (uint)request.Scope,
            Components = (uint)request.Components,
            Expansion = (uint)request.Expansion,
            EntityCount = (uint)roots.Length,
        };
        ComponentChangesNative.WatchStatus st = default;
        st.Size = (uint)sizeof(ComponentChangesNative.WatchStatus);
        int rc;
        fixed (uint* ids = roots)
        {
            rq.Entities = roots.Length == 0 ? null : ids;
            rc = t->Watch(owner, &rq, &st);
        }
        Check(rc, "ComponentChanges.Watch");
        if (st.WatchId == 0) return new ComponentWatchResult(false, ComponentChangesNative.Text(st.Reason, 192), null, (int)st.Pairs);
        var watch = new ComponentWatch(st.WatchId, owner, mod, request, (int)st.Pairs);
        watch.Update(st.Status, null, st.LastCompleteStep, (int)st.Pairs);
        Registry[watch.Id] = watch;
        Interlocked.Increment(ref _watchCount);
        if (mod is not null)
            lock (Mods)
                if (Mods.Add(mod)) mod.Retiring += () => Release(mod);
        StartPump();
        return new ComponentWatchResult(true, null, watch, (int)st.Pairs);
    }

    /// <summary>Resumes a suspended watch from scratch: its roots must still be the same lives, and it must fit the
    /// budgets again. A fresh baseline is read at the next step's start; nothing that changed while it was suspended is
    /// reported. A ready watch answers Ok at once; an ended one answers false. Pre or Post on the tick thread.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="watch"/> is null.</exception>
    /// <exception cref="KilnUnavailableException"><c>events.component_net</c> is off.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or inside a world program's update.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Capability)]
    public static ComponentWatchResult Resume(ComponentWatch watch)
    {
        ArgumentNullException.ThrowIfNull(watch);
        Services.RequireTickThread("ComponentChanges.Resume");
        if (watch.State == ComponentWatchState.Disposed) return new ComponentWatchResult(false, watch.Reason ?? "the watch ended", watch, 0);
        var t = Use();
        ComponentChangesNative.WatchStatus st = default;
        st.Size = (uint)sizeof(ComponentChangesNative.WatchStatus);
        var rc = t->Resume(watch.Owner, watch.Id, &st);
        if (rc == ComponentChangesNative.NotFound)
        {
            watch.Update((uint)ComponentWatchState.Disposed, "the watch ended", watch.LastCompleteStep, 0);
            Forget(watch, remove: false);
            return new ComponentWatchResult(false, "the watch ended", watch, 0);
        }
        Check(rc, "ComponentChanges.Resume");
        var reason = st.Status == (uint)ComponentWatchState.Ready ? null : ComponentChangesNative.Text(st.Reason, 192);
        watch.Update(st.Status, reason, st.LastCompleteStep, (int)st.Pairs);
        if (st.Status == (uint)ComponentWatchState.Disposed) Forget(watch, remove: false);
        return new ComponentWatchResult(st.Status == (uint)ComponentWatchState.Ready, reason, watch, (int)st.Pairs);
    }

    // ------------------------------------------------------------------------------------------------ the request ----

    private static uint[] Validate(ComponentWatchRequest request)
    {
        ArgumentNullException.ThrowIfNull(request.Entities, nameof(request));
        var c = (int)request.Components;
        if (c == 0 || (c & ~(int)ObservedComponents.All) != 0)
            throw new ArgumentException("Components must name one or more of Health, Stamina, Mana, Fuel, Inventory, SlotSelection", nameof(request));
        if (request.Expansion is not (ComponentWatchExpansion.None or ComponentWatchExpansion.PlayerInventoryAndItems))
            throw new ArgumentException($"no expansion {request.Expansion}", nameof(request));
        switch (request.Scope)
        {
            case ComponentWatchScope.Players:
                if (request.Entities.Count != 0) throw new ArgumentException("scope Players follows the players in the game: give no entities", nameof(request));
                return [];
            case ComponentWatchScope.Entities:
                if (request.Entities.Count is 0 or > ComponentChangesNative.MaxRoots)
                    throw new ArgumentException($"scope Entities needs 1 to {ComponentChangesNative.MaxRoots} entities, not {request.Entities.Count}", nameof(request));
                var ids = new uint[request.Entities.Count];
                var seen = new HashSet<uint>();
                for (var i = 0; i < ids.Length; i++)
                {
                    var e = request.Entities[i];
                    if (e.IsNone) throw new ArgumentException("an entity of the list is none", nameof(request));
                    if (!seen.Add(e.Value)) throw new ArgumentException($"entity {e.Value} is listed twice", nameof(request));
                    ids[i] = e.Value;
                }
                return ids;
            default:
                throw new ArgumentException($"no scope {request.Scope}", nameof(request));
        }
    }

    private static void Check(int rc, string api)
    {
        switch (rc)
        {
            case ComponentChangesNative.Ok:
                return;
            case ComponentChangesNative.WrongThread:
                throw new InvalidOperationException($"{api} must be called in Pre or Post on the tick thread, not inside a world program's update");
            case ComponentChangesNative.InvalidArgument:
                throw new ArgumentException($"{api}: kiln.component_changes refused the request");
            default:
                throw ServiceCaps.Unavailable(Capability, $"kiln.component_changes answered {rc}");
        }
    }

    // ------------------------------------------------------------------------------------------------ the tables ----

    private static readonly object Gate = new();
    private static nint _table;
    private static object? _backend;
    private static ulong _owner;
    private static bool _attached;
    private static int _registered;
    private static int _watchCount;
    private static IDisposable? _tick;
    private static int _tickGeneration = -1;
    private static ulong _deliveredBatch;
    private static byte[] _buffer = [];
    internal static readonly ConcurrentDictionary<ulong, ComponentWatch> Registry = new();
    private static readonly HashSet<ModContext> Mods = new(ReferenceEqualityComparer.Instance);

    /// <summary>The event types need <see cref="Capability"/> (subscribing where it is off throws, naming it). Once.</summary>
    internal static void Ensure()
    {
        if (Interlocked.Exchange(ref _registered, 1) != 0) return;
        EventProducers.Register(typeof(ComponentValueChangedEvent), Capability, Off);
        EventProducers.Register(typeof(AttributeValueChangedEvent), Capability, Off);
        EventProducers.Register(typeof(ComponentWatchStateChangedEvent), Capability, Off);
    }

    private static string? Off() => Table() == null ? "kiln.component_changes is not loaded" : null;

    internal static ComponentChangesNative.Table* Table()
    {
        var backend = Backend.Current;
        if (!ReferenceEquals(backend, Volatile.Read(ref _backend)))
        {
            lock (Gate)
            {
                _backend = backend;
                _table = 0;
                _owner = 0;
                _attached = false;
                _deliveredBatch = 0;
                Interlocked.Exchange(ref _watchCount, 0);
                Registry.Clear();  // the watches of another process (a test's last harness)
                lock (Mods) Mods.Clear();
            }
        }
        var t = (ComponentChangesNative.Table*)Volatile.Read(ref _table);
        if (t != null) return t;
        lock (Gate)
        {
            if (_table != 0) return (ComponentChangesNative.Table*)_table;
            t = Services.Get<ComponentChangesNative.Table>(ComponentChangesNative.Service, ComponentChangesNative.Version);
            if (t == null || t->Header.Size < ComponentChangesNative.V1Size) return null;
            Volatile.Write(ref _table, (nint)t);
            return t;
        }
    }

    // The table for a call of a mod: the row on, the dispatcher attached (in Pre or Post, where the service takes it).
    private static ComponentChangesNative.Table* Use()
    {
        Ensure();
        if (!Capabilities.Is(Capability)) throw ServiceCaps.Unavailable(Capability, "named component watches are off here");
        var t = Table();
        if (t == null) throw ServiceCaps.Unavailable(Capability, "kiln.component_changes is not loaded");
        Bind(t);
        return t;
    }

    private static bool Bind(ComponentChangesNative.Table* t)
    {
        if (_attached) return true;
        if (_owner == 0)
        {
            _owner = Backend.Current.OwnerCreate("kiln.component_changes.managed");
            if (_owner == 0) return false;
        }
        var rc = t->Attach(_owner);
        if (rc == ComponentChangesNative.Ok) _attached = true;
        else if (rc != ComponentChangesNative.WrongThread)
            Backend.Current.Log(0, LogLevel.Warn, $"kiln component changes: kiln.component_changes took no managed consumer ({rc}); watches are not delivered");
        return _attached;
    }

    private static void StartPump()
    {
        lock (Gate)
        {
            if (_tick is not null && _tickGeneration == ServiceTick.Generation) return;
            _tickGeneration = ServiceTick.Generation;
            _tick = ServiceTick.Add("kiln component changes", static (in TickInfo t) => Pump(t.Step));
        }
    }

    // A mod unloads: its watches end (the service ends them as well when its owner is revoked).
    // The service ends every watch of the mod's owner in one call (release_owner); here they are only forgotten, so
    // nothing of the mod (its watch objects, their requests) stays referenced and its load context can be collected.
    // The service also ends them by itself when it finds the owner revoked (about once a second), as a backstop.
    private static void Release(ModContext mod)
    {
        lock (Mods) Mods.Remove(mod);
        var n = 0;
        foreach (var w in Registry.Values)
            if (ReferenceEquals(w.Mod, mod))
            {
                w.Update((uint)ComponentWatchState.Disposed, "its mod unloaded", w.LastCompleteStep, 0);
                Forget(w, remove: false);
                n++;
            }
        var t = (ComponentChangesNative.Table*)Volatile.Read(ref _table);
        var rc = t != null && mod.Owner != 0 ? t->ReleaseOwner(mod.Owner) : ComponentChangesNative.Unavailable;
        if (n > 0)
            Backend.Current.Log(0, LogLevel.Info, $"kiln component changes: {mod.Info.Id} unloads: {n} watch(es) released by its owner " +
                (rc == ComponentChangesNative.Ok ? "" : $"(kiln.component_changes answered {rc}: it ends them when it finds the owner revoked)"));
    }

    internal static void Forget(ComponentWatch watch, bool remove)
    {
        if (Registry.TryRemove(watch.Id, out _)) Interlocked.Decrement(ref _watchCount);
        if (!remove) return;
        var t = (ComponentChangesNative.Table*)Volatile.Read(ref _table);
        if (t != null) t->Remove(watch.Owner, watch.Id);  // any thread: it only takes the service's lock
    }

    // ---------------------------------------------------------------------------------------------------- the pump ----

    /// <summary>One Pre: the watches' states, then the changes of the steps since the last Pre. Tick thread. Nothing
    /// while no watch exists.</summary>
    internal static void Pump(ulong step)
    {
        if (!Watching && _deliveredBatch == 0) return;
        var t = Table();
        if (t == null || !Bind(t)) return;
        Poll(t);
        Deliver(t);
    }

    private static void Poll(ComponentChangesNative.Table* t)
    {
        foreach (var w in Registry.Values)
        {
            ComponentChangesNative.WatchStatus st = default;
            st.Size = (uint)sizeof(ComponentChangesNative.WatchStatus);
            var rc = t->Status(w.Id, &st);
            if (rc == ComponentChangesNative.NotFound) st.Status = (uint)ComponentWatchState.Disposed;
            else if (rc != ComponentChangesNative.Ok) continue;
            var reason = st.Status == (uint)ComponentWatchState.Ready ? null
                : rc == ComponentChangesNative.NotFound ? "the watch ended" : ComponentChangesNative.Text(st.Reason, 192);
            if (w.Update(st.Status, reason, st.LastCompleteStep, (int)st.Pairs) && Events.HasSubscribers<ComponentWatchStateChangedEvent>())
                Events.Post(new ComponentWatchStateChangedEvent(w.Id, (ComponentWatchState)st.Status, reason, st.LastCompleteStep));
            if (st.Status == (uint)ComponentWatchState.Disposed) Forget(w, remove: false);
        }
    }

    private static void Deliver(ComponentChangesNative.Table* t)
    {
        ulong batch = 0;
        uint count = 0;
        if (t->BeginEvents(_owner, &batch, &count) != ComponentChangesNative.Ok || batch == 0)
        {
            _deliveredBatch = 0;
            return;
        }
        if (batch != _deliveredBatch && count > 0)
        {
            var stride = sizeof(ComponentChangesNative.Change);
            var bytes = (long)count * stride;
            if (_buffer.Length < bytes) _buffer = GC.AllocateUninitializedArray<byte>(checked((int)bytes), pinned: true);
            var records = (ComponentChangesNative.Change*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(_buffer));
            uint required = 0;
            var rc = t->ReadEvents(_owner, batch, records, count, (uint)stride, &required);
            if (rc != ComponentChangesNative.Ok) return;  // nothing consumed: the same batch at the next Pre
            _deliveredBatch = batch;  // an ack that fails brings the same batch again: its handlers do not run twice
            var raw = Events.HasSubscribers<ComponentValueChangedEvent>();
            var typed = Events.HasSubscribers<AttributeValueChangedEvent>();
            if (raw || typed)
                for (uint i = 0; i < count; i++) Post(records + i, raw, typed);
        }
        else _deliveredBatch = batch;
        if (t->AckEvents(_owner, batch) == ComponentChangesNative.Ok) _deliveredBatch = 0;
    }

    internal static void Post(ComponentChangesNative.Change* r, bool raw, bool typed)
    {
        var e = ToEvent(r);
        if (e is null) return;
        if (raw) Events.Post(e);
        if (typed && r->Size >= ComponentChangesNative.TypedEnd && r->Attribute is >= 1 and <= 4 && (r->ValueBefore != r->ValueAfter || r->MaximumBefore != r->MaximumAfter))
            Events.Post(new AttributeValueChangedEvent(e, (ObservedAttribute)r->Attribute, r->ValueBefore, r->ValueAfter, r->MaximumBefore,
                r->MaximumAfter));
    }

    /// <summary>The event of one record (a pinned record: its fixed buffers are read through the pointer).</summary>
    internal static ComponentValueChangedEvent? ToEvent(ComponentChangesNative.Change* r)
    {
        if (r->Size < ComponentChangesNative.ChangeV1Size || r->Version != 1 || r->Component is < 1 or > 6) return null;
        var before = new ReadOnlySpan<byte>(r->Before, (int)Math.Min(r->BeforeBytes, 256u)).ToArray();
        var after = new ReadOnlySpan<byte>(r->After, (int)Math.Min(r->AfterBytes, 256u)).ToArray();
        var tokens = new ReadOnlySpan<ulong>(r->Tokens, (int)Math.Min(r->TokenCount, 32u)).ToArray();
        var watches = new ReadOnlySpan<ulong>(r->Watches, (int)Math.Min(r->WatchCount, 32u)).ToArray();
        var roots = new EntityId[Math.Min(r->RootCount, 128u)];
        for (var i = 0; i < roots.Length; i++) roots[i] = new EntityId(r->Roots[i]);
        var source = ComponentChangesNative.Text(r->SourceMod, 128);
        return new ComponentValueChangedEvent(r->Entity.ToPublic(), ComponentChangesNative.Names[r->Component], before, after, r->Step,
            (ChangeOrigin)Math.Min(r->Origin, 3u), source.Length == 0 ? null : source, tokens, roots, watches);
    }

    // ----------------------------------------------------------------------------------- what Kiln's writers tell ----

    /// <summary>Whether any managed watch exists in this process (any thread, lock free): Kiln's managed writers keep the bytes
    /// a watch needs only then.</summary>
    internal static bool Watching => Volatile.Read(ref _watchCount) > 0;

    /// <summary>The watched component index (1..6) of a game component name when a managed watch exists and the
    /// service's filter may watch it on <paramref name="entity"/> now, else 0: a managed writer keeps the bytes before a
    /// write only then. Any thread, lock free, no allocation.</summary>
    internal static uint WatchedIndex(uint entity, string component)
    {
        if (!Watching) return 0;
        var index = ComponentChangesNative.IndexOf(component);
        var t = (ComponentChangesNative.Table*)Volatile.Read(ref _table);
        if (index == 0 || t == null) return 0;
        if (t->Header.Size < ComponentChangesNative.WatchedEnd || t->Watched == null) return index;  // no filter: every one
        return (t->Watched(entity) & (1u << (int)(index - 1))) != 0 ? index : 0;
    }

    /// <summary>A program wrap's stage changed a component through an entity view (Kiln.Systems): the whole component
    /// before and after, the wrap's mod. Any thread (slice threads, workers too); cheap when nothing is watched.</summary>
    internal static void NoteWrapWrite(uint entity, string component, ReadOnlySpan<byte> before, ReadOnlySpan<byte> after, string modId) =>
        NoteWrapWrite(entity, ComponentChangesNative.IndexOf(component), before, after, modId);

    internal static void NoteWrapWrite(uint entity, uint index, ReadOnlySpan<byte> before, ReadOnlySpan<byte> after, string modId)
    {
        if (!Watching) return;
        var t = (ComponentChangesNative.Table*)Volatile.Read(ref _table);
        var owner = _owner;
        if (t == null || owner == 0 || before.Length != after.Length || before.Length is 0 or > 256 || string.IsNullOrEmpty(modId)) return;
        if (index is 0 or > 6) return;
        Span<byte> id = stackalloc byte[128];
        var n = Encoding.UTF8.GetByteCount(modId);
        if (n > 127) return;  // the service would call it Unknown
        Encoding.UTF8.GetBytes(modId, id);
        fixed (byte* b = before, a = after, s = id)
        {
            var w = new ComponentChangesNative.KnownWrite
            {
                Size = (uint)sizeof(ComponentChangesNative.KnownWrite),
                Component = index,
                Before = b,
                After = a,
                Bytes = (uint)before.Length,
                SourceMod = new NativeStr { Ptr = s, Len = (nuint)n },
            };
            w.Entity.Entity = entity;
            t->RecordWrite(owner, &w);
        }
    }

    // ------------------------------------------------------------------------------------------------ lab and tests ----

    /// <summary>Lab only (mods/tests/component-watch-test): the service's counters as one line of <c>name=value</c>
    /// pairs (watches, pairs, present pairs, queued frames, overflows, tokens, the window calls' p50, p99 and maximum in
    /// nanoseconds over the last 1024 windows with pairs), or null without the service. Not for mods.</summary>
    [Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [AnyThread]
    public static string? LabCounters()
    {
        if (ReadCounters() is not { } c) return null;
        return $"state={c.State} watches={c.Watches} pairs={c.Pairs} present={c.PresentPairs} queued_frames={c.QueuedFrames} " +
               $"queued_records={c.QueuedRecords} windows={c.Windows} changes={c.Changes} overflows={c.QueueOverflows} " +
               $"frames_dropped={c.FramesDropped} tokens={c.TokensRecorded} tokens_unwatched={c.TokensUnwatched} " +
               $"tokens_refused={c.TokensRefused} tokens_lost={c.TokensLost} tokens_truncated={c.TokensTruncated} " +
               $"tokens_off_world_thread={c.TokensOffWorldThread} begin_p50_ns={c.BeginNsP50} begin_p99_ns={c.BeginNsP99} " +
               $"begin_max_ns={c.BeginNsMax} end_p50_ns={c.EndNsP50} end_p99_ns={c.EndNsP99} end_max_ns={c.EndNsMax} samples={c.Samples}";
    }

    /// <summary>The service's counters (kiln_component_changes.h counters()), null without the table or the member.
    /// Labs and tests (the observer's cost: window_begin and window_end p50 and p99).</summary>
    internal static ComponentChangesNative.Counters? ReadCounters()
    {
        var t = Table();
        if (t == null || t->Header.Size < ComponentChangesNative.CountersEnd) return null;
        ComponentChangesNative.Counters c = default;
        c.Size = (uint)sizeof(ComponentChangesNative.Counters);
        return t->CountersFn(&c) == ComponentChangesNative.Ok ? c : null;
    }

    /// <summary>Test hook: forget the table, the owner and every watch (a harness ends).</summary>
    internal static void Reset()
    {
        lock (Gate)
        {
            _table = 0;
            _owner = 0;
            _attached = false;
            _deliveredBatch = 0;
            _tick?.Dispose();
            _tick = null;
            _tickGeneration = -1;
            Registry.Clear();
            Interlocked.Exchange(ref _watchCount, 0);
            lock (Mods) Mods.Clear();
        }
    }
}

/// <summary>Mirrors of <c>kiln_component_changes.h</c> (v1 with its appended members). The sizes are the header's,
/// pinned by a test.</summary>
internal static unsafe class ComponentChangesNative
{
    public const string Service = "kiln.component_changes";
    public const uint Version = 1;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5;
    public const int MaxWatches = 32, MaxPairs = 4096, MaxRoots = 128;
    public const uint TypedEnd = 1792; // end of the appended typed values, before SinceStep
    public const uint ChangeV1Size = 1768;  // the S1 prefix; the service writes 1808 (the appended fields)
    public static readonly uint V1Size = (uint)(sizeof(ServiceHeader) + 13 * sizeof(nint));
    public static readonly uint CountersEnd = (uint)(sizeof(ServiceHeader) + 16 * sizeof(nint));
    public static readonly uint WatchedEnd = (uint)(sizeof(ServiceHeader) + 17 * sizeof(nint));
    public static readonly string[] Names = ["", "Health", "Stamina", "Mana", "Fuel", "Inventory", "SlotSelection"];

    public static uint IndexOf(string component)
    {
        var name = component.AsSpan();
        if (name.StartsWith("keen::ecs::", StringComparison.Ordinal)) name = name["keen::ecs::".Length..];
        for (uint i = 1; i < Names.Length; i++)
            if (name.SequenceEqual(Names[i])) return i;
        return 0;
    }

    public static string Text(byte* p, int max)
    {
        var n = 0;
        while (n < max && p[n] != 0) n++;
        return Encoding.UTF8.GetString(p, n);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WatchRequest
    {
        public uint Size, Scope, Components, Expansion;
        public uint* Entities;
        public uint EntityCount, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WatchStatus
    {
        public uint Size, Status, Pairs, Limit;
        public ulong WatchId, LastCompleteStep;
        public fixed byte Reason[192];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Change
    {
        public uint Size, Version, Kind, Component;
        public ulong WorldEpoch, Step;
        public DecisionsNative.EntityKey Entity;
        public uint BeforeBytes, AfterBytes, Origin, Flags;
        public uint RootCount, WatchCount, TokenCount, Reserved;
        public fixed uint Roots[128];
        public fixed ulong Watches[32];
        public fixed ulong Tokens[32];
        public fixed byte Before[256];
        public fixed byte After[256];
        public fixed byte SourceMod[128];
        // appended (0.2.0-dev.29)
        public uint WorldKind, Attribute;
        public int ValueBefore, ValueAfter, MaximumBefore, MaximumAfter;
        public ulong SinceStep, WorldSession;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KnownWrite
    {
        public uint Size, Component;
        public ulong WorldEpoch, Step, OperationToken;
        public DecisionsNative.EntityKey Entity;
        public byte* Before, After;
        public uint Bytes, Reserved;
        public NativeStr SourceMod;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Counters
    {
        public uint Size, State;
        public uint Watches, Pairs, PresentPairs, QueuedRecords, QueuedFrames, Reserved;
        public ulong WorldEpoch, LastCompleteStep, Windows, Changes, QueueOverflows, FramesDropped;
        public ulong TokensRecorded, TokensUnwatched, TokensRefused, TokensLost, TokensTruncated, TokensOffWorldThread;
        public ulong BeginNsP50, BeginNsP99, EndNsP50, EndNsP99, Samples;
        public ulong BeginNsMax, EndNsMax;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, int> Attach;
        public delegate* unmanaged[Cdecl]<ulong, int> Detach;
        public delegate* unmanaged[Cdecl]<ulong, WatchRequest*, WatchStatus*, int> Watch;
        public delegate* unmanaged[Cdecl]<ulong, ulong, WatchStatus*, int> Resume;
        public delegate* unmanaged[Cdecl]<ulong, WatchStatus*, int> Status;
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> Remove;
        public delegate* unmanaged[Cdecl]<ulong, int> ReleaseOwner;
        public delegate* unmanaged[Cdecl]<ulong, ulong*, uint*, int> BeginEvents;
        public delegate* unmanaged[Cdecl]<ulong, ulong, Change*, uint, uint, uint*, int> ReadEvents;
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> AckEvents;
        public delegate* unmanaged[Cdecl]<ulong, KnownWrite*, int> RecordWrite;
        public delegate* unmanaged[Cdecl]<ulong, ulong, void> WindowBegin;
        public delegate* unmanaged[Cdecl]<ulong, ulong, void> WindowEnd;
        // appended (0.2.0-dev.29): read only when Header.Size covers them
        public delegate* unmanaged[Cdecl]<uint> WindowWanted;
        public delegate* unmanaged[Cdecl]<uint, void> WindowFaulted;
        public delegate* unmanaged[Cdecl]<Counters*, int> CountersFn;
        public delegate* unmanaged[Cdecl]<uint, uint> Watched;
    }
}
