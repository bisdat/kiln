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
}

/// <summary>A session-only player crafting source. Dispose or unload its registering mod to remove it. In-flight
/// payments may finish. After a restart the mod must register its sources again. Since 0.2.0-dev.30.6.</summary>
public sealed class CraftSourceHandle : IDisposable
{
    private long _handle;
    private readonly ulong _owner;
    private readonly ModContext? _mod;
    internal CraftSourceHandle(ulong owner, ulong handle, EntityId player, EntityId container, ModContext? mod)
    {
        _owner = owner; _handle = unchecked((long)handle); Player = player; Container = container; _mod = mod;
        if (mod is not null) mod.Retiring += Dispose;
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
        finally { if (_mod is not null) _mod.Retiring -= Dispose; }
    }
}

/// <summary>Player crafting sources for station, NPC and inventory crafting. Since 0.2.0-dev.30.6.</summary>
public static class Crafting
{
    /// <summary>Server payment through the recipe transaction, with player-bound linked storage.</summary>
    public const string SourcesCapability = "crafting.sources";
    /// <summary>A Kiln client's crafting screens count the server's linked-storage snapshot.</summary>
    public const string SourceUiCapability = "crafting.source_ui";

    /// <summary>Registers a container for this player, in any base or none. The mod decides which containers the player
    /// may use; the server rechecks the player's own global CanAccessInventories permission and a readable initialized
    /// container at payment time. It does not enforce base ownership. Player/NPC inventories are refused. Sources pay
    /// only a shortfall after the bag and crafting base's stock, in one all-or-nothing recipe transaction. Equal-order
    /// sources use registration order; each container is counted once per item. A vanilla client keeps its own counts.</summary>
    /// <returns>A session-only handle. Dispose or unload the registering mod to remove it; re-register after restart.</returns>
    /// <exception cref="ArgumentException">Invalid player, non-container, or zero item id.</exception>
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
        var handle = CraftingCore.Add(owner, player, container, o.Order, o.Filter, items);
        return new CraftSourceHandle(owner, handle, player, container, mod);
    }
}

internal static unsafe class CraftingCore
{
    private static StationsNative.Table* Table => Services.Get<StationsNative.Table>(StationsNative.Name, 1);
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
    internal static ulong Add(ulong owner, EntityId player, EntityId container, CraftSourceOrder order, CraftSourceFilter filter, uint[] items)
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
                Items = p
            };
            rc = t->AddSource(owner, &r, &id);
        }
        if (rc == StationsNative.InvalidArgument) throw new ArgumentException("Crafting.AddSource: player or initialized storage container is invalid (player/NPC inventories are not containers)");
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
