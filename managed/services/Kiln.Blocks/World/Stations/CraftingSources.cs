// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>Where linked storage pays, after the bag and the crafting base's stock. Since 0.2.0-dev.30.6.</summary>
public enum CraftSourceOrder
{
    /// <summary>Before opted-in station outputs (the default).</summary>
    BeforeStationOutputs = 0,
    /// <summary>After opted-in station outputs.</summary>
    AfterStationOutputs = 1,
}

/// <summary>How <see cref="CraftSourceOptions.Items"/> is interpreted. Since 0.2.0-dev.30.6.</summary>
public enum CraftSourceFilter
{
    /// <summary>Every item except the set; an empty set allows everything (the default).</summary>
    Deny = 0,
    /// <summary>Only items in the set; an empty set allows nothing.</summary>
    Allow = 1,
}

/// <summary>Options copied when a linked crafting source is registered. Since 0.2.0-dev.30.6.</summary>
public sealed class CraftSourceOptions
{
    /// <summary>After the bag/base stock, before or after opted-in station outputs.</summary>
    public CraftSourceOrder Order { get; init; }
    /// <summary>Allow-list or deny-list interpretation of <see cref="Items"/>; default deny.</summary>
    public CraftSourceFilter Filter { get; init; }
    /// <summary>The item set, copied eagerly on registration; null means an empty set. No managed predicate runs in
    /// the native payment hook, which may run on a game worker thread. Register again to change the filter.</summary>
    public IReadOnlyCollection<ItemId>? Items { get; init; }
    /// <summary>Locks too (default false): a lock that requires an item (the game's InteractionLock: a lockpick, a key)
    /// also counts this container, through <see cref="Filter"/> and <see cref="Items"/>, and a lock that uses the item
    /// up takes it from here after the player's bag, in link order, in the game's own all-or-nothing interaction
    /// transaction. A lock that only needs the item to be held takes nothing. Needs capability
    /// <see cref="Crafting.LockSourcesCapability"/> on the server or host: the server's own lock check decides every
    /// player's press, so the lock opens for a vanilla client too, but only a Kiln client's locked prompt and predicted
    /// press count the link (a vanilla client still shows the lock locked and may first play the failed attempt).
    /// Without the capability the link still serves crafting. Since 0.2.0-dev.31.12, mod API 0.2.21.</summary>
    public bool Locks { get; init; }
}

/// <summary>A session-only player crafting source. Dispose or unload its registering mod to remove it. In-flight
/// payments may finish. After a restart the mod must register its sources again. Since 0.2.0-dev.30.6.</summary>
public sealed class CraftSourceHandle : IDisposable
{
    private long _handle;
    private readonly ulong _owner;
    private readonly ModContext? _mod;
    private readonly bool _locks;
    private readonly object? _lockSession;  // the lock session this handle counts in (GitHub #147)
    internal CraftSourceHandle(ulong owner, ulong handle, EntityId player, EntityId container, ModContext? mod, bool locks = false)
    {
        _owner = owner; _handle = unchecked((long)handle); Player = player; Container = container; _mod = mod; _locks = locks;
        if (mod is not null) mod.Retiring += Dispose;
        if (locks) _lockSession = LockStock.Track(player);
    }
    /// <summary>The only player this registration can supply.</summary>
    public EntityId Player { get; }
    /// <summary>The registered storage container, independent of its base.</summary>
    public EntityId Container { get; }
    /// <summary>Whether this handle has been disposed.</summary>
    public bool IsDisposed => Interlocked.Read(ref _handle) == 0;
    /// <summary>Removes this registration, on any thread. Idempotent; no other registration is removed.</summary>
    [AnyThread]
    public void Dispose()
    {
        var id = Interlocked.Exchange(ref _handle, 0);
        if (id == 0) return;
        try { CraftingCore.Remove(_owner, unchecked((ulong)id)); }
        finally
        {
            if (_mod is not null) _mod.Retiring -= Dispose;
            if (_locks) LockStock.Untrack(Player, _lockSession);
        }
    }
}

/// <summary>Player crafting sources for station, NPC and inventory crafting. Since 0.2.0-dev.30.6.</summary>
public static class Crafting
{
    /// <summary>Server payment through the recipe transaction, with player-bound linked storage.</summary>
    public const string SourcesCapability = "crafting.sources";
    /// <summary>A Kiln client's crafting screens count the server's linked-storage snapshot.</summary>
    public const string SourceUiCapability = "crafting.source_ui";
    /// <summary>Locks count and pay their required item from links registered with <see cref="CraftSourceOptions.Locks"/>:
    /// on a server or host its lock check and lock payment, on a Kiln client its lock check and locked prompt (from the
    /// server's snapshot). Off, or faulted, when a symbol, a hook or the game's code it reads is missing or patched by
    /// another module (kiln.log names it). Since 0.2.0-dev.31.12, mod API 0.2.21.</summary>
    public const string LockSourcesCapability = "items.lock_sources";

    /// <summary>Registers a container for this player, in any base or none. The mod decides which containers the player
    /// may use; the server rechecks the player's own global CanAccessInventories permission and a readable initialized
    /// container at payment time. It does not enforce base ownership. Player/NPC inventories and production stations
    /// are refused; production stations lend their outputs through <see cref="Stations.SetOutputSource"/>. Sources pay
    /// only a shortfall after the bag and crafting base's stock, in one all-or-nothing recipe transaction. Equal-order
    /// sources use registration order; each container is counted once per item. A vanilla client keeps its own counts.
    /// Since 0.2.0-dev.31.12, mod API 0.2.21, <see cref="CraftSourceOptions.Locks"/> lets the same link supply a lock's
    /// required item.</summary>
    /// <returns>A session-only handle. Dispose or unload the registering mod to remove it; re-register after restart.</returns>
    /// <exception cref="ArgumentException">Invalid player, non-container, production station, or zero item id.</exception>
    /// <exception cref="ArgumentOutOfRangeException">Unknown option or more than 65536 filter items.</exception>
    /// <exception cref="InvalidOperationException">Wrong thread, unloading mod, or 4096 registrations already exist.</exception>
    /// <exception cref="KilnUnavailableException">Wrong side, missing native service/payment symbols, offline or no server session.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(SourcesCapability)]
    public static CraftSourceHandle AddSource(EntityId player, EntityId container, CraftSourceOptions? options = null)
    {
        Services.RequireTickThread("Crafting.AddSource");
        if (player.IsNone || player.Value > 1023) throw new ArgumentException("player must be a player entity (1..1023)", nameof(player));
        if (container.IsNone) throw new ArgumentException("no container", nameof(container));
        var o = options ?? new CraftSourceOptions();
        if (o.Order is not (CraftSourceOrder.BeforeStationOutputs or CraftSourceOrder.AfterStationOutputs)) throw new ArgumentOutOfRangeException(nameof(options), "unknown source order");
        if (o.Filter is not (CraftSourceFilter.Allow or CraftSourceFilter.Deny)) throw new ArgumentOutOfRangeException(nameof(options), "unknown item filter");
        if (o.Items?.Count > 65536) throw new ArgumentOutOfRangeException(nameof(options), "at most 65536 filter items");
        uint[] items = o.Items?.Select(i => i.Value).Distinct().Order().ToArray() ?? [];
        if (items.Contains(0u)) throw new ArgumentException("filter contains no item", nameof(options));
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var owner = Services.CallerOwner;
        if (o.Locks) CraftingCore.NoteLocks();
        var handle = CraftingCore.Add(owner, player, container, o.Order, o.Filter, items, o.Locks);
        return new CraftSourceHandle(owner, handle, player, container, mod, o.Locks);
    }
}

internal static unsafe class CraftingCore
{
    // Only the prefix through SourceStock is required; later appended members must not enlarge this requirement.
    internal static bool HasSourcePrefix(uint size) => size >= (uint)(StationsNative.TableFillUi + 4 * sizeof(nint));
    private static StationsNative.Table* Table
    {
        get
        {
            var header = Services.Get<ServiceHeader>(StationsNative.Name, 1);
            return header != null && HasSourcePrefix(header->Size) ? (StationsNative.Table*)header : null;
        }
    }
    internal static bool HasSources => Table is var t && t != null && t->AddSource != null && t->RemoveSource != null && t->SourceStock != null;
    private static readonly ServiceCapRows Rows = new(StationsNative.Name, () =>
    {
        if (!HasSources) ServiceCaps.Static(Crafting.SourcesCapability, false, CapabilityReason.ServiceOlder,
            "kiln.stations has no player crafting-source members", StationsNative.Name);
    });
    private static StationsNative.Table* Need()
    {
        Rows.Ensure();
        if (!Capabilities.Is(Crafting.SourcesCapability)) throw ServiceCaps.Unavailable(Crafting.SourcesCapability, "Crafting.AddSource cannot run");
        var t = Table;
        if (t == null || !HasSources) throw new KilnUnavailableException(Crafting.SourcesCapability, "kiln.stations has no player crafting-source members");
        return t;
    }
    // GitHub #147: kiln.stations' lock members (set_lock_stock, lock_asks) and the request's flags came in one release.
    internal static bool HasLockMembers(uint size) => size >= (uint)StationsNative.TableLocks;
    internal static bool HasLocks => Table is var t && t != null && HasLockMembers(t->Header.Size) && t->SetLockStock != null && t->LockAsks != null;
    private static int _locksNoted;
    // A link asked for Locks: say once why locks will not use it when they cannot (the link still serves crafting).
    internal static void NoteLocks()
    {
        if (!HasLocks)
            ServiceCaps.Static(Crafting.LockSourcesCapability, false, CapabilityReason.ServiceOlder,
                "kiln.stations has no lock-source members (older kiln-items.dll)", StationsNative.Name);
        if (Capabilities.Is(Crafting.LockSourcesCapability) || Interlocked.Exchange(ref _locksNoted, 1) != 0) return;
        ModLog.Warn($"crafting: a source asked for Locks, but locks do not use linked sources here (the link still serves crafting): " +
                    Capabilities.Why(Crafting.LockSourcesCapability));
    }
    internal static ulong Add(ulong owner, EntityId player, EntityId container, CraftSourceOrder order, CraftSourceFilter filter, uint[] items,
        bool locks = false)
    {
        var t = Need(); ulong id = 0; int rc;
        fixed (uint* p = items)
        {
            var r = new StationsNative.SourceRequest
            {
                Size = (uint)sizeof(StationsNative.SourceRequest),
                Player = player.Value,
                Container = container.Value,
                Order = (uint)order,
                Filter = (uint)filter,
                ItemCount = (uint)items.Length,
                Items = p,
                Flags = locks && HasLocks ? StationsNative.SourceLocks : 0u,
            };
            rc = t->AddSource(owner, &r, &id);
        }
        if (rc == StationsNative.InvalidArgument) throw new ArgumentException("Crafting.AddSource: player or initialized storage container is invalid (player/NPC inventories are not containers; production stations lend their outputs through Stations.SetOutputSource)");
        if (rc == StationsNative.BufferTooSmall) throw new InvalidOperationException("Crafting.AddSource: 4096 sources are registered already");
        if (rc != StationsNative.Ok || id == 0) throw new KilnUnavailableException(Crafting.SourcesCapability, "Crafting.AddSource: no server session, or native payment/access checks are unavailable");
        return id;
    }
    internal static void Remove(ulong owner, ulong handle)
    {
        var t = Table; if (t != null && t->RemoveSource != null) t->RemoveSource(owner, handle);
    }
    internal static (int Rc, StationsNative.Stack[] Rows) ExtraStock(EntityId player) => Stock(player, 0);
    internal static (int Rc, StationsNative.Stack[] Rows) AllStock(EntityId player) => Stock(player, 1);
    /// <summary>The lock-opted links' counts alone (no bag, no base; GitHub #147).</summary>
    internal static (int Rc, StationsNative.Stack[] Rows) LockStock(EntityId player) =>
        HasLocks ? Stock(player, StationsNative.StockLocks) : (StationsNative.Unavailable, []);
    internal static int SetLockStock(bool on, StationsNative.Stack[] rows)
    {
        if (!HasLocks) return StationsNative.Unavailable;
        fixed (StationsNative.Stack* p = rows) return Table->SetLockStock(on ? 1u : 0u, p, (uint)rows.Length);
    }
    internal static (int Rc, uint[] Items) LockAsks()
    {
        if (!HasLocks) return (StationsNative.Unavailable, []);
        var items = new uint[StationsNative.MaxLockAsks]; uint n = 0;
        int rc;
        fixed (uint* p = items) rc = Table->LockAsks(p, (uint)items.Length, &n);
        return rc == StationsNative.Ok && n <= items.Length ? (rc, items[..(int)n]) : (rc, []);
    }
    private static (int Rc, StationsNative.Stack[] Rows) Stock(EntityId player, uint flags)
    {
        if (!HasSources || !Capabilities.Is(Crafting.SourcesCapability)) return (StationsNative.Unavailable, []);
        var rows = new StationsNative.Stack[StationsNative.MaxCraftStock]; uint n = 0;
        fixed (StationsNative.Stack* p = rows)
        {
            var rc = Table->SourceStock(player.Value, flags, p, (uint)rows.Length, &n);
            if (rc != StationsNative.Ok || n > rows.Length) return (rc == StationsNative.Ok ? StationsNative.BufferTooSmall : rc, []);
        }
        return (StationsNative.Ok, rows[..(int)n]);
    }
}
