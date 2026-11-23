// SPDX-License-Identifier: MIT
// Items.FindInventoryOwners (0.2.0-dev.31, GitHub issue #138): the inventory owners Items.FindInventories finds, each with
// the kind Kiln can prove for it (kiln.items.inventory_kinds, native/core/abi/services/kiln_items_inventory_kinds.h).
// Capability items.inventory_kinds. docs/kiln/services/items.md "Inventory owners and their kinds".
namespace Kiln;

/// <summary>What an inventory owner is, as far as Kiln can prove it (<see cref="Items.FindInventoryOwners"/>). Stable codes,
/// only ever added to; a code this build does not know reads as <see cref="Unknown"/>. Since 0.2.0-dev.31.</summary>
public enum InventoryOwnerKind : uint
{
    /// <summary>Nothing proves what it is. Ordinary storage chests are here: no component of the game tells a chest from
    /// any other plain container, and Kiln does not guess from template names. Also a base's storage while the game's
    /// bases cannot be read (kiln.stations' <c>stations.read</c> off).</summary>
    Unknown = 0,
    /// <summary>A player's inventory (the game's InventorySetup flag IsPlayer).</summary>
    Player = 1,
    /// <summary>An NPC's inventory, a villager's (the game's InventorySetup flag IsNpc).</summary>
    Npc = 2,
    /// <summary>The holder of a player's storage (<c>Items.RegisterPlayerStorage</c>); only with
    /// <see cref="InventoryKinds.PlayerStorage"/>.</summary>
    PlayerStorage = 3,
    /// <summary>A production station (it carries the game's <c>FactoryStation</c>, as <c>Stations</c> reads it): a forge,
    /// a kiln, a loom.</summary>
    ProductionStation = 4,
    /// <summary>In a base's crafting stock as the game lists it this frame (<c>Bases.Storage</c>): a Magic storage chest,
    /// or a player-planted refilling fruit tree or palm (the game marks both the same way).</summary>
    BaseStorage = 5,
}

/// <summary>An inventory owner and its kind (<see cref="Items.FindInventoryOwners"/>). Since 0.2.0-dev.31.</summary>
/// <param name="Entity">The entity that owns the inventory.</param>
/// <param name="Kind">What it is, as far as Kiln can prove it.</param>
public readonly record struct InventoryOwnerInfo(EntityId Entity, InventoryOwnerKind Kind);

public static unsafe partial class Items
{
    /// <summary>The capability row of <see cref="FindInventoryOwners"/>: the kiln.items.inventory_kinds table is here.</summary>
    public const string InventoryKindsCapability = "items.inventory_kinds";

    /// <summary>
    /// The entities with an initialized inventory of these kinds, as <see cref="FindInventories"/> finds them (the same
    /// flags, the same order, player storage holders only with <see cref="InventoryKinds.PlayerStorage"/>), each with what
    /// Kiln can prove it is (<see cref="InventoryOwnerKind"/>). One pass over the world's entities and one read of the game's
    /// bases. Since 0.2.0-dev.31 (GitHub issue #138).
    /// </summary>
    /// <remarks>An ordinary storage chest is <see cref="InventoryOwnerKind.Unknown"/>: nothing in the game's data tells it
    /// apart, and Kiln does not guess. The base's chests: <c>Bases.Chests</c>. <see cref="FindInventories"/> is unchanged.</remarks>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException"><see cref="InventoryKindsCapability"/> is off (no kiln.items.inventory_kinds
    /// table: an older kiln-items.dll), or kiln.items cannot read here.</exception>
    [RequiresCapability(InventoryKindsCapability)]
    [TickThreadOnly]
    public static IReadOnlyList<InventoryOwnerInfo> FindInventoryOwners(InventoryKinds kinds = InventoryKinds.Containers)
    {
        Services.RequireTickThread("Items.FindInventoryOwners");
        Rows.Ensure();
        var t = InventoryKindsNative.T;
        if (t == null) throw ServiceCaps.Unavailable(InventoryKindsCapability, "kiln.items.inventory_kinds is not loaded");
        // without kiln.items.storage there is no holder to find (as FindInventories)
        if ((kinds & InventoryKinds.PlayerStorage) != 0 && StorageNative.T == null)
        {
            kinds &= ~InventoryKinds.PlayerStorage;
            if ((kinds & InventoryKinds.All) == 0) return [];
        }
        var cap = 4096u;
        while (true)
        {
            var buf = new InventoryKindsNative.Row[cap];
            uint n;
            int rc;
            fixed (InventoryKindsNative.Row* p = buf) rc = t->Find((uint)kinds, p, cap, &n);
            if (rc == ItemsNative.BufferTooSmall && cap < (1u << 20)) { cap = Math.Max(cap * 2, n + 64); continue; }
            if (rc != ItemsNative.Ok && rc != ItemsNative.BufferTooSmall) throw Failure("Items.FindInventoryOwners", rc);
            var list = new List<InventoryOwnerInfo>((int)Math.Min(n, cap));
            for (var i = 0; i < n && i < cap; i++) list.Add(new InventoryOwnerInfo(new EntityId(buf[i].Entity), KindOf(buf[i].Kind)));
            return list;
        }
    }

    /// <summary>A native kind code; one this build does not know is Unknown (the codes are only added to).</summary>
    internal static InventoryOwnerKind KindOf(uint code) => code <= (uint)InventoryOwnerKind.BaseStorage ? (InventoryOwnerKind)code : InventoryOwnerKind.Unknown;
}
