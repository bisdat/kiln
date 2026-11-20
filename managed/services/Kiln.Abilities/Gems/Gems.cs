// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>Enshrouded's gems for mods: the catalog, the gems a player's equipment runs, sockets, new gems with their
/// own effects (<c>Register</c>), the forge's actions and gem events. docs/kiln/services/gems.md.</summary>
public static partial class Gems
{
    /// <summary>Every gem item: the game's and those mods added (items of category <see cref="ItemCategory.WeaponGem"/>),
    /// with the program and configs of their ItemInfo as they are now (a data pack's change included). Read again when
    /// the item catalog or the game data changed, at most every 2 seconds otherwise. Tick thread; works on a server and
    /// in a game client with a world (kiln.items' catalog and kiln.gamedata).</summary>
    /// <exception cref="KilnUnavailableException"><c>gems.read</c>: kiln.items' catalog is not here.</exception>
    [TickThreadOnly]
    [RequiresCapability(GemsCaps.Read)]
    public static IReadOnlyList<GemInfo> Catalog
    {
        get
        {
            Services.RequireTickThread("Gems.Catalog");
            GemsCaps.Ensure();
            try { return GemReads.Catalog(GemReads.World); }
            catch (KilnUnavailableException e) { throw GemsCaps.AsRead(e); }
        }
    }

    /// <summary>One gem item's definition, or null when the item is not a gem. Tick thread.</summary>
    [TickThreadOnly]
    [RequiresCapability(GemsCaps.Read)]
    public static GemInfo? Get(ItemId gem)
    {
        Services.RequireTickThread("Gems.Get");
        GemsCaps.Ensure();
        try { return GemReads.Get(GemReads.World, gem); }
        catch (KilnUnavailableException e) { throw GemsCaps.AsRead(e); }
    }

    /// <summary>The gems whose effects run for a player now, one per equipment impact slot with a socketed gem (the
    /// held item, shield, armour, rings, ammo, spell), in slot order: the game's <c>Equipment.equipmentImpacts</c> where
    /// it spawned a gem impact. Empty for an entity with no equipment, and with no server world (a joined client, the
    /// menu). Server (and a game client that hosts the world). Tick thread.</summary>
    /// <exception cref="KilnUnavailableException"><c>gems.read</c>: item instances or the game's reflection of
    /// Equipment are not here.</exception>
    [TickThreadOnly]
    [RequiresCapability(GemsCaps.Read)]
    public static IReadOnlyList<EquippedGem> Equipped(EntityId player)
    {
        Services.RequireTickThread("Gems.Equipped");
        GemsCaps.Ensure();
        try { return GemReads.Equipped(GemReads.World, player); }
        catch (KilnUnavailableException e) { throw GemsCaps.AsRead(e); }
    }

    /// <summary>The gem in an item's socket, or null when it has no socket, the socket is empty, or the item is gone.
    /// Server (and a hosting client). Tick thread.</summary>
    /// <exception cref="KilnUnavailableException"><c>gems.read</c>: item instances are not here, or no server world
    /// runs (a joined client, the menu).</exception>
    [TickThreadOnly]
    [RequiresCapability(GemsCaps.Read)]
    public static SocketedGem? Socket(ItemInstanceRef carrier)
    {
        Services.RequireTickThread("Gems.Socket");
        GemsCaps.Ensure();
        if (carrier.IsNone) return null;
        try { return GemReads.Socket(GemReads.World, carrier); }
        catch (KilnUnavailableException e) { throw GemsCaps.AsRead(e); }
    }
}

/// <summary>The read API's logic over <see cref="IGemWorld"/> (tests give it a fake).</summary>
internal static class GemReads
{
    internal static IGemWorld World { get; set; } = new LiveGemWorld();

    private static IReadOnlyList<GemInfo> _catalog = [];
    private static Dictionary<uint, GemInfo> _byId = new();
    private static long _key = long.MinValue / 4;
    private static long _readAt = long.MinValue / 4;
    private static IGemWorld? _for;

    /// <summary>How long a catalog is kept when nothing says it changed (data packs' value edits do not always change
    /// the key).</summary>
    internal static long MaxAgeMs { get; set; } = 2000;

    /// <summary>Drops the cached catalog (tests, a world change).</summary>
    internal static void Forget()
    {
        _catalog = [];
        _byId = new();
        _key = long.MinValue / 4;
        _readAt = long.MinValue / 4;
        _keyCheckedAt = long.MinValue / 4;
        _for = null;
        _gemIds = [];
        _gemIdsOf = null;
        _gemIdsCount = -1;
    }

    /// <summary>How long a catalog is used without asking for its key again (a poll asks for every item it reads).</summary>
    internal static long KeyCheckMs { get; set; } = 100;

    private static long _keyCheckedAt = long.MinValue / 4;

    internal static IReadOnlyList<GemInfo> Catalog(IGemWorld world)
    {
        var now = Environment.TickCount64;
        var same = ReferenceEquals(_for, world) && now - _readAt < MaxAgeMs;
        if (same && now - _keyCheckedAt < KeyCheckMs) return _catalog;
        var key = world.CatalogKey();
        _keyCheckedAt = now;
        if (same && key == _key) return _catalog;
        var mods = world.ModItems();
        var names = world.ModNames();  // the item catalog names the game's items only
        var list = new List<GemInfo>();
        foreach (var item in world.Items())
        {
            if (item.Category != ItemCategory.WeaponGem || item.IsBlueprint) continue;
            var data = world.Data(item);
            var shown = item.DisplayName is { Length: > 0 } d ? d : names.GetValueOrDefault(item.Id.Value) is { Length: > 0 } n ? n : null;
            list.Add(new GemInfo(item.Id, item.Name, shown, item.Rarity, item.LevelMin, item.LevelMax,
                data?.Program ?? Guid.Empty, data?.Configs ?? new Dictionary<uint, GemConfigValue>(),
                mods.Contains(item.Id.Value) || data?.IsClone == true));
        }
        _catalog = list;
        _byId = list.ToDictionary(g => g.Item.Value);
        _key = key;
        _readAt = now;
        _for = world;
        return _catalog;
    }

    internal static GemInfo? Get(IGemWorld world, ItemId gem)
    {
        Catalog(world);
        return _byId.GetValueOrDefault(gem.Value);
    }

    private static HashSet<uint> _gemIds = [];
    private static IReadOnlyList<ItemInfo>? _gemIdsOf;
    private static int _gemIdsCount = -1;

    /// <summary>The ids of the gem items (category WeaponGem, not a blueprint: the catalog's own rule) from the item
    /// catalog alone, kept while kiln.items keeps its list. What the socket poll asks for every item it reads: it needs
    /// no program or config, so it never builds the catalog (kiln.gamedata's JSON for every gem, the config vocabulary)
    /// and a game-data failure cannot stop it (review).</summary>
    internal static IReadOnlySet<uint> GemIds(IGemWorld world)
    {
        var items = world.Items();
        if (ReferenceEquals(items, _gemIdsOf) && items.Count == _gemIdsCount) return _gemIds;
        var set = new HashSet<uint>();
        foreach (var item in items)
            if (item.Category == ItemCategory.WeaponGem && !item.IsBlueprint) set.Add(item.Id.Value);
        _gemIds = set;
        _gemIdsOf = items;
        _gemIdsCount = items.Count;
        return set;
    }

    internal static bool IsGem(IGemWorld world, ItemId item) => !item.IsNone && GemIds(world).Contains(item.Value);

    internal static IReadOnlyList<EquippedGem> Equipped(IGemWorld world, EntityId player)
    {
        if (player.IsNone) return [];
        var rows = world.Impacts(player);
        if (rows is null) return [];
        var withGem = rows.Where(r => !r.GemImpact.IsNone && r.Slot <= (int)GemImpactSlot.Spell).ToList();
        if (withGem.Count == 0) return [];
        // one read for every carrier and every gem entity
        var pides = new List<uint>(withGem.Count * 2);
        foreach (var r in withGem)
        {
            pides.Add(r.CarrierPide);
            pides.Add(r.GemEntity.Value);
        }
        var reads = world.ReadEntities(pides);
        var result = new List<EquippedGem>(withGem.Count);
        for (var i = 0; i < withGem.Count; i++)
        {
            var r = withGem[i];
            var carrier = reads[2 * i];
            var gemEntity = reads[2 * i + 1];
            // the carrier's record says which gem and level; the gem's own entity when the carrier cannot be read
            var gem = carrier is { Gem.IsNone: false } c ? c.Gem : gemEntity?.Item.Item ?? ItemId.None;
            var level = carrier is { Gem.IsNone: false } c2 ? c2.GemLevel : gemEntity?.Level ?? 0;
            var socket = new SocketedGem(carrier?.Item ?? ItemInstanceRef.None, gem, level);
            result.Add(new EquippedGem(player, (GemImpactSlot)r.Slot, socket, r.GemImpact));
        }
        return result;
    }

    internal static SocketedGem? Socket(IGemWorld world, ItemInstanceRef carrier) =>
        world.Read(carrier) is { Gem.IsNone: false } r ? new SocketedGem(carrier, r.Gem, r.GemLevel) : null;
}
