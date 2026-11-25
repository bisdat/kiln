// SPDX-License-Identifier: MIT
// Player storage (since 0.2.0-dev.30.7, mod API 0.2.16; GitHub issue 128, thanks Fuzen777): storages a mod registers for
// every player (a quiver, a spellbook, an extra bag) that survive a world save, a rejoin, a trip to another world, the mod
// being off and Kiln being removed, using witness-paired copies and bounded histories. Native: kiln.items.storage
// (native/core/abi/services/kiln_items_storage.h, served by kiln-items.dll); persistence: Kiln.Saves. The runtime that
// talks to the table is PlayerStorageRuntime.cs. Usage: docs/kiln/services/player-storage.md.
using System.Collections.Immutable;
using System.Text.RegularExpressions;

namespace Kiln;

/// <summary>What happens to a storage's items when its player dies. Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
public enum StorageDeath
{
    /// <summary>The storage keeps its items (the default).</summary>
    Keep = 0,
    /// <summary>Every stack goes into the player's tombstone, when the game made one.</summary>
    Grave = 1,
    /// <summary>The game's own tombstone rule decides, as for the backpack: "Everything" takes every stack,
    /// "AddBackpackMaterials" only materials and collectibles, "NoTombstone" nothing. Kiln never makes a tombstone itself:
    /// when the game made none, the storage keeps its items.</summary>
    FollowGame = 2,
}

/// <summary>Kinds of ammunition, for <see cref="StorageFilter.Ammunition"/>. Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
[Flags]
public enum AmmoKinds
{
    /// <summary>Items the game equips into the arrow slot.</summary>
    Arrow = 1,
    /// <summary>Items the game equips into the spell slot.</summary>
    Spell = 2,
}

/// <summary>
/// What items a player storage takes. The default value takes any item. Combine with <see cref="And"/>: every part must
/// hold. Since 0.2.0-dev.30.7 (mod API 0.2.16).
/// </summary>
/// <remarks>
/// <para>Kiln checks the filter natively on every add into a storage (a give, a move, a restore): an item it does not take
/// is refused with <see cref="ItemsStatus.Filtered"/>, and at a restore it waits parked.</para>
/// <para><see cref="Where"/> is evaluated <b>once</b>, over the item catalog, into a list of item ids: when the storage is
/// registered if the server world has loaded its items, else as soon as it has (until then the storage is not ready).
/// That is because the check runs inside kiln.items' program during the game's simulation step, on the game's own thread
/// and inside its inventory transaction, where no mod code may run; a list of ids is checked there without calling back,
/// and gives the same answer on every call. An item added to the catalog later in the session is not in the list.</para>
/// </remarks>
public readonly record struct StorageFilter
{
    private readonly uint _categories;                       // bit per ItemCategory; 0: any
    private readonly AmmoKinds _ammo;                        // 0: any
    private readonly bool _noItemEntities;
    private readonly ImmutableArray<ItemId> _items;          // default: no id list
    private readonly ImmutableArray<Func<ItemInfo, bool>> _where;  // default: none

    private StorageFilter(uint categories, AmmoKinds ammo, bool noItemEntities, ImmutableArray<ItemId> items,
        ImmutableArray<Func<ItemInfo, bool>> where)
    {
        _categories = categories;
        _ammo = ammo;
        _noItemEntities = noItemEntities;
        _items = items;
        _where = where;
    }

    /// <summary>Any item.</summary>
    public static StorageFilter Any => default;

    /// <summary>Items of these categories.</summary>
    /// <exception cref="ArgumentException">No category, or a value that is no category.</exception>
    public static StorageFilter Categories(params ItemCategory[] categories)
    {
        ArgumentNullException.ThrowIfNull(categories);
        if (categories.Length == 0) throw new ArgumentException("at least one category", nameof(categories));
        uint bits = 0;
        foreach (var c in categories)
        {
            if (!Enum.IsDefined(c)) throw new ArgumentException($"{(int)c} is no item category", nameof(categories));
            bits |= 1u << (int)c;
        }
        return new StorageFilter(bits, 0, false, default, default);
    }

    /// <summary>Exactly these items.</summary>
    /// <exception cref="ArgumentException">No item, item 0, or more than 1024 items.</exception>
    public static StorageFilter Items(params ItemId[] items)
    {
        ArgumentNullException.ThrowIfNull(items);
        if (items.Length == 0) throw new ArgumentException("at least one item", nameof(items));
        if (items.Any(i => i.IsNone)) throw new ArgumentException("item 0 is no item", nameof(items));
        var list = items.Distinct().OrderBy(i => i.Value).ToImmutableArray();
        if (list.Length > StorageNative.MaxFilterIds) throw new ArgumentException($"at most {StorageNative.MaxFilterIds} items", nameof(items));
        return new StorageFilter(0, 0, false, list, default);
    }

    /// <summary>Ammunition of these kinds (what the game equips into the arrow or spell slot).</summary>
    /// <exception cref="ArgumentException">No kind, or a value outside <see cref="AmmoKinds"/>.</exception>
    public static StorageFilter Ammunition(AmmoKinds kinds)
    {
        if (kinds == 0 || (kinds & ~(AmmoKinds.Arrow | AmmoKinds.Spell)) != 0)
            throw new ArgumentException("Arrow, Spell or both", nameof(kinds));
        return new StorageFilter(0, kinds, false, default, default);
    }

    /// <summary>Items <paramref name="pick"/> says yes to, asked once per catalog item (see the remarks of
    /// <see cref="StorageFilter"/>): keep it a pure function of the item.</summary>
    public static StorageFilter Where(Func<ItemInfo, bool> pick)
    {
        ArgumentNullException.ThrowIfNull(pick);
        return new StorageFilter(0, 0, false, default, [pick]);
    }

    /// <summary>Only items both filters take.</summary>
    /// <exception cref="ArgumentException">The two leave no item (no common category, ammunition kind or item).</exception>
    public StorageFilter And(StorageFilter other)
    {
        var categories = _categories == 0 ? other._categories : other._categories == 0 ? _categories : _categories & other._categories;
        if (_categories != 0 && other._categories != 0 && categories == 0) throw new ArgumentException("the filters have no category in common", nameof(other));
        var ammo = _ammo == 0 ? other._ammo : other._ammo == 0 ? _ammo : _ammo & other._ammo;
        if (_ammo != 0 && other._ammo != 0 && ammo == 0) throw new ArgumentException("the filters have no ammunition kind in common", nameof(other));
        var items = _items.IsDefault ? other._items : other._items.IsDefault ? _items : [.. _items.Intersect(other._items)];
        if (!_items.IsDefault && !other._items.IsDefault && items.Length == 0) throw new ArgumentException("the filters have no item in common", nameof(other));
        var where = _where.IsDefault ? other._where : other._where.IsDefault ? _where : _where.AddRange(other._where);
        return new StorageFilter(categories, ammo, _noItemEntities || other._noItemEntities, items, where);
    }

    /// <summary>The same filter, taking only items without an item entity: plain stackables, no gear.</summary>
    public StorageFilter NoItemEntities() => new(_categories, _ammo, true, _items, _where);

    /// <summary>The filter needs the item catalog (it has a <see cref="Where"/>).</summary>
    internal bool NeedsCatalog => !_where.IsDefault;

    internal uint CategoryBits => _categories;
    internal AmmoKinds Ammo => _ammo;
    internal bool NoPide => _noItemEntities;

    /// <summary>The native form: categories, flags and the sorted id list (a <see cref="Where"/> evaluated over
    /// <paramref name="catalog"/>, within the explicit items and categories when there are some).</summary>
    /// <exception cref="ArgumentException">The list would hold more than 1024 items, or no item at all.</exception>
    internal (uint Categories, uint Flags, uint[] Ids) Compile(IReadOnlyList<ItemInfo> catalog)
    {
        var flags = (_ammo.HasFlag(AmmoKinds.Arrow) ? StorageNative.FilterAmmoArrow : 0u) |
                    (_ammo.HasFlag(AmmoKinds.Spell) ? StorageNative.FilterAmmoSpell : 0u) |
                    (_noItemEntities ? StorageNative.FilterNoPide : 0u);
        uint[] ids;
        if (_where.IsDefault) ids = _items.IsDefault ? [] : [.. _items.Select(i => i.Value)];
        else
        {
            var allowed = _items.IsDefault ? null : _items.ToHashSet();
            var picks = _where;
            var categories = _categories;
            var noPide = _noItemEntities;
            ids = [.. catalog.Where(i => (allowed is null || allowed.Contains(i.Id)) &&
                                         (categories == 0 || (categories & (1u << (int)i.Category)) != 0) &&
                                         (!noPide || !i.IsGear) && picks.All(p => p(i)))
                             .Select(i => i.Id.Value).Distinct().Order()];
            if (ids.Length == 0) throw new ArgumentException("the filter's Where takes no item of the catalog");
        }
        if (ids.Length > StorageNative.MaxFilterIds)
            throw new ArgumentException($"the filter takes {ids.Length} items; a list holds at most {StorageNative.MaxFilterIds} (narrow it, e.g. with Categories)");
        return (_categories, flags, ids);
    }

    /// <summary>The filter in words, for a log line.</summary>
    public override string ToString()
    {
        var parts = new List<string>();
        var bits = _categories;
        if (bits != 0)
            parts.Add("categories " + string.Join("|", Enum.GetValues<ItemCategory>().Where(c => (bits & (1u << (int)c)) != 0)));
        if (_ammo != 0) parts.Add($"ammunition {_ammo}");
        if (!_items.IsDefault) parts.Add($"{_items.Length} item(s)");
        if (!_where.IsDefault) parts.Add($"{_where.Length} Where");
        if (_noItemEntities) parts.Add("no item entities");
        return parts.Count == 0 ? "any item" : string.Join(", ", parts);
    }
}

/// <summary>Where a player's storage stands. Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
public enum PlayerStorageState
{
    /// <summary>The player has no such storage (no registration, nothing parked; or no such player).</summary>
    None = 0,
    /// <summary>Registered; the player's storages are not restored yet (the join is in progress, or the server cannot save
    /// storages here, so none is built).</summary>
    Waiting = 1,
    /// <summary>Restored: the storage's inventory exists and takes items.</summary>
    Ready = 2,
    /// <summary>No mod registered this key (or Kiln holds its items back): its items wait, unchanged.</summary>
    Parked = 3,
    /// <summary>This character's storages are read-only for the session (they were saved by a newer Kiln).</summary>
    Locked = 4,
}

/// <summary>Which copy a player's storages were restored from. Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
public enum PlayerStorageSource
{
    /// <summary>Not restored.</summary>
    None = 0,
    /// <summary>No copy anywhere: the storages started empty.</summary>
    New = 1,
    /// <summary>The world's own copy.</summary>
    Server = 2,
    /// <summary>The copy that travels with the character (the player's game).</summary>
    Client = 3,
}

/// <summary>Why a storage's items wait parked. Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
public enum PlayerStorageParkReason
{
    /// <summary>No loaded mod registered the key (the mod is off). They come back when it registers again.</summary>
    Unregistered = 1,
    /// <summary>More items than the registration has slots (it got smaller), or the slot was taken. They come back when
    /// there is room.</summary>
    Overflow = 2,
    /// <summary>The registration's filter no longer takes the item.</summary>
    Filter = 3,
    /// <summary>Held back on request (an admin, Kiln.Saves).</summary>
    Requested = 16,
    /// <summary>Held back: the storage changed elsewhere since this server's copy, and this server does not take the copy
    /// the player brought. An admin can release it.</summary>
    Stale = 17,
    /// <summary>Held back: saved by a newer Kiln.</summary>
    Locked = 18,
    /// <summary>The server held its copy because the character's items were likely returned outside Kiln. An admin
    /// should release it only if that is wrong, through <see cref="Items.PlayerStorages.ReturnOrphans"/>.</summary>
    ReturnedOutsideKiln = 19,
}

/// <summary>One player's storage of one key (<see cref="PlayerStorage.Status"/>). Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
/// <param name="State">Where it stands.</param>
/// <param name="Paired">The restored copy was saved together with exactly the character the player brought (the safe
/// case); false: Kiln took the newest copy (the character changed outside Kiln).</param>
/// <param name="Source">The copy it was restored from.</param>
/// <param name="Stacks">Stacks in the storage now.</param>
/// <param name="Parked">Records of this key waiting parked.</param>
/// <param name="Quarantined">Records of this key Kiln could not use (an unknown item, a bad count or slot); kept, never
/// deleted.</param>
/// <param name="Revision">The restored copy's revision.</param>
public sealed record PlayerStorageStatus(PlayerStorageState State, bool Paired, PlayerStorageSource Source, int Stacks, int Parked,
    int Quarantined, long Revision)
{
    /// <summary>The storage is restored and takes items.</summary>
    public bool Restored => State == PlayerStorageState.Ready;

    internal static readonly PlayerStorageStatus NoneYet = new(PlayerStorageState.None, false, PlayerStorageSource.None, 0, 0, 0, 0);
}

/// <summary>One key of a player's storages, any mod's (<see cref="Items.PlayerStorages.Of"/>). Since 0.2.0-dev.30.7.</summary>
/// <param name="FullKey">"&lt;mod id&gt;:&lt;name&gt;".</param>
/// <param name="Registered">A loaded mod registered it.</param>
/// <param name="Slots">The registration's slots (0 when unregistered).</param>
/// <param name="Holder">The storage's inventory (none until restored).</param>
/// <param name="Status">Where it stands.</param>
public sealed record PlayerStorageInfo(string FullKey, bool Registered, int Slots, EntityId Holder, PlayerStorageStatus Status);

/// <summary>Where <see cref="Items.PlayerStorages.ReturnOrphans"/> puts parked items. Since 0.2.0-dev.30.7.</summary>
public readonly record struct OrphanTarget
{
    private OrphanTarget(EntityId container) => Container = container;

    /// <summary>The player's backpack (as far as there is room).</summary>
    public static OrphanTarget Backpack => default;

    /// <summary>An inventory owner: a chest, a station.</summary>
    /// <exception cref="ArgumentException">No entity.</exception>
    public static OrphanTarget Into(EntityId container) =>
        container.IsNone ? throw new ArgumentException("no container", nameof(container)) : new OrphanTarget(container);

    /// <summary>The container; none for the backpack.</summary>
    public EntityId Container { get; }

    /// <summary>The player's backpack.</summary>
    public bool IsBackpack => Container.IsNone;

    /// <summary>The target in words, for a log line.</summary>
    public override string ToString() => IsBackpack ? "backpack" : $"container {Container}";
}

/// <summary>
/// One storage a mod registered (<see cref="Items.RegisterPlayerStorage"/>): every player of the server world has one. Its
/// inventory is a real game inventory Kiln owns, not part of the player's backpack, so the game never draws it, saves it
/// with the character, crafts from it or buries it; Kiln saves it, paired with the character. Since 0.2.0-dev.30.7 (mod
/// API 0.2.16).
/// </summary>
/// <remarks>
/// <para>Use <see cref="EntityOf"/> with every Kiln.Items call that takes an inventory owner: <see cref="Items.Read"/>,
/// <see cref="Items.Give(EntityId, ItemId, int, Gear?, GiveOptions, SlotId?)"/>, <see cref="Items.Take"/>,
/// <see cref="Items.Move"/>, <see cref="Items.MoveAll"/>; or <see cref="Store"/> and <see cref="Retrieve"/>. Gear moves with
/// its item entity (level, upgrades, gem, dye, durability). The storage is not one of the player's inventories:
/// <see cref="Items.Read"/> of the player and crafting do not see it, and <see cref="Items.FindInventories"/> lists it only
/// with <see cref="InventoryKinds.PlayerStorage"/>. Players cannot drag items in or out themselves: only the server moves
/// them (a mod's command, a menu of the mod's, the mod's client sending a wish).</para>
/// <para>A storage is ready for a player after Kiln restored it at the join (<see cref="IsReady"/>,
/// <see cref="PlayerStorageRestoredEvent"/>); before that, and on a game client that joined another's world, it is not.
/// Reads and moves: the tick thread.</para>
/// </remarks>
public sealed class PlayerStorage
{
    /// <summary>The capability row of player storage: <c>items.storage</c>.</summary>
    public const string Capability = "items.storage";

    internal readonly ulong Owner;
    internal readonly ModContext? Mod;
    internal uint Handle;                 // 0 until kiln.items.storage took the registration
    internal string? Refusal;             // why it was not taken (a deferred filter that failed)

    internal PlayerStorage(string key, string fullKey, int slots, StorageFilter filter, StorageDeath death, ulong owner, ModContext? mod)
    {
        Key = key;
        FullKey = fullKey;
        Slots = slots;
        Filter = filter;
        Death = death;
        Owner = owner;
        Mod = mod;
    }

    /// <summary>Whether player storage works on this game build (capability <c>items.storage</c>);
    /// <c>Capabilities.Why(PlayerStorage.Capability)</c> says why not.</summary>
    [AnyThread]
    public static bool Available => Capabilities.Is(Capability);

    /// <summary>The key the mod gave.</summary>
    public string Key { get; }

    /// <summary>"&lt;mod id&gt;:&lt;key&gt;": the storage's name in saves, logs and <see cref="Items.PlayerStorages"/>.</summary>
    public string FullKey { get; }

    /// <summary>How many slots it has (1 to 64).</summary>
    public int Slots { get; }

    /// <summary>What it takes.</summary>
    public StorageFilter Filter { get; }

    /// <summary>What happens to its items when the player dies.</summary>
    public StorageDeath Death { get; }

    /// <summary>The storage is restored for <paramref name="player"/> and takes items. False before, after the player left,
    /// and where player storage does not work. Any thread.</summary>
    [AnyThread]
    public bool IsReady(EntityId player) => Status(player).State == PlayerStorageState.Ready;

    /// <summary>The storage's inventory owner for <paramref name="player"/> (use it with Kiln.Items' calls), or none while it
    /// is not ready.</summary>
    [TickThreadOnly]
    public EntityId EntityOf(EntityId player) => PlayerStorageRuntime.HolderOf(this, player, "PlayerStorage.EntityOf");

    /// <summary>What the storage holds for <paramref name="player"/> (<see cref="Items.Read"/> of <see cref="EntityOf"/>), or
    /// null while it is not ready.</summary>
    [TickThreadOnly]
    public InventoryContents? Read(EntityId player)
    {
        var holder = EntityOf(player);
        return holder.IsNone ? null : Items.Read(holder);
    }

    /// <summary>Slot <paramref name="index"/> (0 to <see cref="Slots"/> - 1) of the storage for <paramref name="player"/>, or
    /// none while it is not ready.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="index"/> is outside the storage.</exception>
    [TickThreadOnly]
    public SlotId Slot(EntityId player, int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(index, Slots);
        var contents = Read(player);
        if (contents is null) return default;
        var page = index / 8;
        return page < contents.Inventories.Count ? SlotId.Of(contents.Inventories[page].Inventory, index % 8) : default;
    }

    /// <summary>Moves a stack (or <paramref name="count"/> of it) from <paramref name="player"/>'s inventory into the storage:
    /// <see cref="Items.Move"/>, with Kiln's choice of slot. An item the filter does not take is
    /// <see cref="ItemsStatus.Filtered"/>; a storage that is not ready is <see cref="ItemsStatus.NoInventory"/>.</summary>
    [TickThreadOnly, RequiresCapability(Items.Caps.Move)]
    public Task<MoveResult> Store(EntityId player, SlotId from, int count = 0)
    {
        var holder = EntityOf(player);
        return holder.IsNone
            ? Task.FromResult(MoveResult.Fail(ItemsStatus.NoInventory, MoveKind.Stack, player, holder, NotReady(player)))
            : Items.Move(player, from, holder, count);
    }

    /// <summary>Moves the stack in slot <paramref name="index"/> (or <paramref name="count"/> of it) out of the storage into
    /// <paramref name="player"/>'s backpack, or into <paramref name="to"/> (a chest): <see cref="Items.Move"/>.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="index"/> is outside the storage.</exception>
    [TickThreadOnly, RequiresCapability(Items.Caps.Move)]
    public Task<MoveResult> Retrieve(EntityId player, int index, int count = 0, EntityId to = default)
    {
        var slot = Slot(player, index);
        var target = to.IsNone ? player : to;
        var holder = slot.IsNone ? EntityId.None : EntityOf(player);
        return holder.IsNone
            ? Task.FromResult(MoveResult.Fail(ItemsStatus.NoInventory, MoveKind.Stack, holder, target, NotReady(player)))
            : Items.Move(holder, slot, target, count);
    }

    /// <summary>
    /// Makes the arrows or spell in slot <paramref name="index"/> the player's active ammunition through the backpack: the
    /// stack moves into the backpack (<see cref="Items.Move"/>) and is equipped there (<see cref="Items.Equip"/>), and the
    /// stack this storage activated before for that slot goes back into the storage. For a game that does not fire from a
    /// stack outside the backpack; where it does, equip the storage slot itself (<see cref="Items.Equip"/> with
    /// <see cref="Slot"/>), which moves nothing. <see cref="PlayerStorageAmmoOptions.Activate"/> makes the HUD's picks use
    /// this. Since 0.2.0-dev.30.7 (mod API 0.2.16).
    /// </summary>
    /// <returns>The equip's result, one step after the last move: <see cref="ItemsStatus.NoInventory"/> while the storage is
    /// not ready, <see cref="ItemsStatus.SlotChanged"/> for an empty slot, <see cref="ItemsStatus.Refused"/> for an item
    /// that is no arrow or spell, or while another <c>Activate</c> of this storage for the same player and slot is under
    /// way, a failed move's status (nothing equipped; <see cref="ItemsStatus.NoRoom"/>: the backpack is full), or the
    /// equip's. When the equip fails, what the move put into the backpack goes back into the storage. A stack that cannot
    /// go back (the storage is full, its filter refuses it) stays in the backpack.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="index"/> is outside the storage.</exception>
    /// <exception cref="KilnUnavailableException">The row <c>items.move</c> or <c>items.equip</c> is off.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(Items.Caps.Move), RequiresCapability(Items.Caps.Equip)]
    public Task<EquipResult> Activate(EntityId player, int index)
    {
        var slot = Slot(player, index);
        if (slot.IsNone)
            return Task.FromResult(EquipResult.Fail(ItemsStatus.NoInventory, player, null, false, slot, NotReady(player), false));
        var stack = Read(player)?.Slots.FirstOrDefault(s => s.Slot == slot);
        if (stack is null || stack.IsEmpty)
            return Task.FromResult(EquipResult.Fail(ItemsStatus.SlotChanged, player, null, false, slot, $"slot {index} of {FullKey} holds nothing", false));
        var kind = PlayerStorageMirror.KindOf(stack.Item);
        if (kind == 0)
            return Task.FromResult(EquipResult.Fail(ItemsStatus.Refused, player, null, false, slot, $"{stack.Item} is no arrow or spell", false));
        if (!PlayerStorageActivation.Hooked)
            foreach (var row in (string[])[Items.Caps.Move, Items.Caps.Equip])
                if (!Capabilities.Is(row)) throw ServiceCaps.Unavailable(row);
        var holder = EntityOf(player);
        return PlayerStorageActivation.Run(this, player, holder, slot, stack, kind);
    }

    // the stack Activate last moved into the backpack, per player and kind: it goes back at the next Activate; and the
    // activations under way (tick thread)
    internal readonly Dictionary<(uint Player, AmmoKinds Kind), (SlotId Cell, ItemId Item)> Activated = [];
    internal readonly HashSet<(uint Player, AmmoKinds Kind)> Activating = [];

    /// <summary>Where the storage stands for <paramref name="player"/>. Any thread.</summary>
    [AnyThread]
    public PlayerStorageStatus Status(EntityId player) => PlayerStorageRuntime.StatusOf(this, player);

    private string NotReady(EntityId player) =>
        $"{FullKey} is not ready for {player}" + (Refusal is { } why ? $" ({why})" : "");

    /// <summary>The storage in one line, for a log.</summary>
    public override string ToString() => $"player storage {FullKey} ({Slots} slots, {Filter}, death {Death})";

    /// <summary>
    /// The player's own read-only copy of their storages on their game: rows the server sends the storage's owner (never
    /// another player) when a storage changes, so a mod's client half can show them (a HUD, a menu), and the HUD's
    /// ammunition lists fed from them (<see cref="ProvideAmmo"/>). Since 0.2.0-dev.30.7 (mod API 0.2.16), capability
    /// <c>items.storage.client</c>.
    /// </summary>
    /// <remarks>
    /// <para>Kiln's channel <c>kiln:storage</c> v1 carries the copy (not required: a game without it joins as before and
    /// gets nothing). The server is the authority: the copy is a picture of what the server holds, a message from a game
    /// never changes a storage, and a game cannot ask for another player's rows. The server sends the whole set of the
    /// player's ready storages (any mod's) about four times a second at most, and only when it changed. A hosting game
    /// fills its own player's copy directly.</para>
    /// <para>The copy is empty until the server restored the player's storages, on a server without Kiln, without the
    /// storage's mod, or where player storage is off; it empties when the game leaves the world.</para>
    /// <para><see cref="Rows"/> is the game's; <see cref="ProvideAmmo"/> is made on both sides (its server half takes the
    /// picks).</para>
    /// </remarks>
    public static class Client
    {
        /// <summary>The capability row of the client copy: <c>items.storage.client</c>.</summary>
        public const string Capability = "items.storage.client";

        /// <summary>Whether the client copy works here (capability <c>items.storage.client</c>: Kiln's network channel is
        /// open).</summary>
        [AnyThread]
        public static bool Available
        {
            get
            {
                PlayerStorageMirror.Start();
                return Capabilities.Is(Capability);
            }
        }

        /// <summary>The local player's rows of storage <paramref name="fullKey"/> (<see cref="PlayerStorage.FullKey"/>), by
        /// slot: one per slot that holds something. Empty while the copy has none of that storage.</summary>
        /// <exception cref="KilnUnavailableException">The row <c>items.storage.client</c> is off.</exception>
        [ClientOnly, AnyThread, RequiresCapability(Capability)]
        public static IReadOnlyList<PlayerStorageRow> Rows(string fullKey)
        {
            ArgumentException.ThrowIfNullOrEmpty(fullKey);
            Items.Rows.Ensure();
            PlayerStorageMirror.Start();
            if (!PlayerStorageMirror.Open) throw ServiceCaps.Unavailable(Capability);
            return PlayerStorageMirror.Store.Rows(fullKey);
        }

        /// <summary>
        /// Shows the arrows and spells of your mod's storage <paramref name="fullKey"/> in the HUD's ammunition and spell
        /// lists (<c>Ui.AmmoList</c>, GitHub issue 127), and takes the player's pick of such a row on the server: it equips
        /// the stack from its storage slot (<see cref="Items.Equip"/>), or with <see cref="PlayerStorageAmmoOptions.Activate"/>
        /// swaps it into the backpack first (<see cref="PlayerStorage.Activate"/>). Both halves of the common case in one
        /// call: make it on both sides, in <c>OnLoad</c> of a mod with side <c>both</c>.
        /// </summary>
        /// <remarks>
        /// <para>On a game client the rows come from the client copy: one row per item (the counts of its slots added up),
        /// in slot order, only items the game equips into that list's slot. They follow the copy as it changes. The HUD's
        /// rules apply (<c>Ui.AmmoList.Provide</c>): in Append the game's own row shows for an item the game lists itself
        /// (the storage's row is kept behind it), and a pick of an item the player carries is the game's own. Every other
        /// pick goes to the server as a wish, since 0.2.0-dev.31.12 also one of such an item once the backpack's stack has
        /// left (GitHub issue 145).</para>
        /// <para>On the server (a dedicated server, or a game hosting the world) the pick is checked like any request from
        /// a game: the storage is the sender's own, the item is in it, it is ammunition of the list's kind; at most 4 picks
        /// a second per player. Then the first slot holding the item is equipped as the storage's mod. A pick that does not
        /// check out is dropped (logged). Nothing else a game sends changes the storage.</para>
        /// <para>One provider per storage at a time. Where there is no HUD (a dedicated server) or <c>ui.ammo.list</c> is off,
        /// only the server half runs (<see cref="PlayerStorageAmmoProvider.Shown"/> is false and says why).</para>
        /// </remarks>
        /// <param name="fullKey">The storage's full key, one of the calling mod's (<c>"&lt;your mod id&gt;:&lt;key&gt;"</c>).
        /// On a game that joined another's world the storage is not registered there; its key is enough.</param>
        /// <param name="kinds">The lists: <see cref="AmmoKinds.Arrow"/>, <see cref="AmmoKinds.Spell"/> or both.</param>
        /// <param name="options">Replace the game's rows, or activate on a pick (<see cref="PlayerStorageAmmoOptions"/>).</param>
        /// <returns>The provider; dispose it to end both halves. It ends with the mod.</returns>
        /// <exception cref="ArgumentException">A bad key, a key of another mod, or no kind.</exception>
        /// <exception cref="InvalidOperationException">Outside mod code, or the storage provides its ammunition already.</exception>
        /// <exception cref="KilnUnavailableException">The row <c>items.storage.client</c> is off.</exception>
        /// <example><code>
        /// // OnLoad, both sides
        /// _ammo = PlayerStorage.Client.ProvideAmmo("quiversample:quiver", AmmoKinds.Arrow | AmmoKinds.Spell);
        /// Log.Info(_ammo.ToString());
        /// </code></example>
        [AnyThread, RequiresCapability(Capability)]
        public static PlayerStorageAmmoProvider ProvideAmmo(string fullKey, AmmoKinds kinds, PlayerStorageAmmoOptions? options = null) =>
            PlayerStorageAmmo.Provide(fullKey, kinds, options);
    }

    // a key's name part: lowercase letters, digits, '.', '_', '-'; 32 at most
    internal static readonly Regex NameRule = new("^[a-z0-9][a-z0-9._-]{0,31}$", RegexOptions.CultureInvariant);

    // a full key: "<mod id>:<name>" (kiln_items_storage.h: mod id 1-64, name 1-32 of the same characters)
    internal static readonly Regex FullKeyRule = new("^[a-z0-9][a-z0-9._-]{0,63}:[a-z0-9][a-z0-9._-]{0,31}$", RegexOptions.CultureInvariant);
}

/// <summary>One slot of a storage in the client copy (<see cref="PlayerStorage.Client.Rows"/>): a picture of what the
/// server holds, never authority. Since 0.2.0-dev.30.7.</summary>
/// <param name="Key">The storage's full key.</param>
/// <param name="Index">The slot (0 to slots - 1), as <see cref="PlayerStorage.Slot"/> counts them.</param>
/// <param name="Item">The item.</param>
/// <param name="Count">How many (at least 1).</param>
/// <param name="Level">Gear's level (0 for a plain stack).</param>
/// <param name="Flags">The item's <see cref="AmmoKinds"/> as the server's item catalog has it: 1 (<see cref="AmmoKinds.Arrow"/>)
/// for arrows, 2 (<see cref="AmmoKinds.Spell"/>) for a spell, 0 for anything else; other bits are 0 in this version. A game
/// that joined another's world has no item catalog of its own (<see cref="Items.Get"/> answers null there): this is how
/// its client half tells ammunition apart.</param>
public readonly record struct PlayerStorageRow(string Key, int Index, ItemId Item, int Count, int Level, uint Flags);

/// <summary>Kiln restored a player's storage of one key at the join (or when a mod registered it while the player was on):
/// it is ready now. Posted on the tick thread. Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
[ServerOnly]
public sealed class PlayerStorageRestoredEvent : KilnEvent
{
    internal PlayerStorageRestoredEvent(EntityId player, Guid character, string key, int stacks, bool paired, PlayerStorageSource source,
        bool live, long revision)
    {
        Player = player;
        Character = character;
        Key = key;
        Stacks = stacks;
        Paired = paired;
        Source = source;
        Live = live;
        Revision = revision;
    }

    /// <summary>The player.</summary>
    public EntityId Player { get; }
    /// <summary>The game's CharacterId of the character.</summary>
    public Guid Character { get; }
    /// <summary>The storage's full key.</summary>
    public string Key { get; }
    /// <summary>Stacks put back.</summary>
    public int Stacks { get; }
    /// <summary>The copy was saved together with exactly this character (see <see cref="PlayerStorageStatus.Paired"/>).</summary>
    public bool Paired { get; }
    /// <summary>The copy it came from.</summary>
    public PlayerStorageSource Source { get; }
    /// <summary>A registration while the player was on (not the join).</summary>
    public bool Live { get; }
    /// <summary>The copy's revision.</summary>
    public long Revision { get; }

    /// <summary>The event in one line, for a log.</summary>
    public override string ToString() =>
        $"player storage {Key} restored for {Player}: {Stacks} stack(s) ({(Paired ? "paired" : "unpaired")}, {Source}{(Live ? ", live" : "")})";
}

/// <summary>Items of a player's storage were parked: they wait, unchanged, in Kiln's copies (<see cref="Reason"/>). Posted
/// on the tick thread. Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
[ServerOnly]
public sealed class PlayerStorageParkedEvent : KilnEvent
{
    internal PlayerStorageParkedEvent(EntityId player, Guid character, string key, int stacks, PlayerStorageParkReason reason)
    {
        Player = player;
        Character = character;
        Key = key;
        Stacks = stacks;
        Reason = reason;
    }

    /// <summary>The player.</summary>
    public EntityId Player { get; }
    /// <summary>The game's CharacterId of the character.</summary>
    public Guid Character { get; }
    /// <summary>The storage's full key.</summary>
    public string Key { get; }
    /// <summary>Stacks parked.</summary>
    public int Stacks { get; }
    /// <summary>Why.</summary>
    public PlayerStorageParkReason Reason { get; }

    /// <summary>The event in one line, for a log.</summary>
    public override string ToString() => $"player storage {Key} of {Player}: {Stacks} stack(s) parked ({Reason})";
}

/// <summary>A player died: what Kiln did with a storage's items (<see cref="StorageDeath"/>). Posted on the tick thread.
/// Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
[ServerOnly]
public sealed class PlayerStorageDeathEvent : KilnEvent
{
    internal PlayerStorageDeathEvent(EntityId player, string key, int moved, int items, EntityId to, StorageDeath rule)
    {
        Player = player;
        Key = key;
        Moved = moved;
        Items = items;
        To = to;
        Rule = rule;
    }

    /// <summary>The player.</summary>
    public EntityId Player { get; }
    /// <summary>The storage's full key.</summary>
    public string Key { get; }
    /// <summary>Stacks moved into the tombstone (0: kept).</summary>
    public int Moved { get; }
    /// <summary>Their items together.</summary>
    public int Items { get; }
    /// <summary>The tombstone; none when the storage kept everything.</summary>
    public EntityId To { get; }
    /// <summary>The storage's death rule.</summary>
    public StorageDeath Rule { get; }

    /// <summary>The event in one line, for a log.</summary>
    public override string ToString() =>
        Moved == 0 ? $"player storage {Key} of {Player} kept its items at death ({Rule})" : $"player storage {Key} of {Player}: {Moved} stack(s) to tombstone {To} ({Rule})";
}

/// <summary>The local player's client copy of a storage changed (<see cref="PlayerStorage.Client"/>): the server sent other
/// rows, the storage went (its rows are then empty), or the game left the world. One event per storage. On the tick thread
/// (a joined game: the main thread; a hosting game: the server thread). Capability <c>items.storage.client</c>. Since
/// 0.2.0-dev.30.7.</summary>
[ClientOnly]
public sealed class PlayerStorageClientChangedEvent : KilnEvent
{
    internal PlayerStorageClientChangedEvent(string key, IReadOnlyList<PlayerStorageRow> rows)
    {
        Key = key;
        Rows = rows;
    }

    /// <summary>The storage's full key.</summary>
    public string Key { get; }
    /// <summary>Its rows now.</summary>
    public IReadOnlyList<PlayerStorageRow> Rows { get; }
}

// Activate's steps (outside the unsafe Items class: C# allows no await in an unsafe context). Each runs on the tick thread
// one step after the one before, as the storage's mod; one at a time per player and equipment slot.
internal static class PlayerStorageActivation
{
    /// <summary>Tests: kiln.items' move and equip (the test kit has neither); null: Items.Move and Items.Equip.</summary>
    internal static Func<EntityId, SlotId, EntityId, int, MoveOptions, Task<MoveResult>>? MoveHook;
    internal static Func<EntityId, SlotId, EquipmentSlot, Task<EquipResult>>? EquipHook;

    /// <summary>Tests: the hooks stand in for items.move and items.equip.</summary>
    internal static bool Hooked => MoveHook is not null && EquipHook is not null;

    public static async Task<EquipResult> Run(PlayerStorage storage, EntityId player, EntityId holder, SlotId slot, InventorySlot stack, AmmoKinds kind)
    {
        var equipSlot = kind == AmmoKinds.Arrow ? EquipmentSlot.Arrow : EquipmentSlot.Spell;
        var key = (player.Value, kind);
        if (!storage.Activating.Add(key))
            return EquipResult.Fail(ItemsStatus.Refused, player, equipSlot, false, slot,
                $"another Activate of {storage.FullKey} for this player's {equipSlot} slot is under way", false);
        // Every await below comes back to the tick thread through the mod's scheduler: the caller may be one of Kiln's own
        // passes (a pick from the player's game), which have no synchronization context, and kiln.items completes its
        // tasks off the step. The async builder gives the caller its own context back when this method first returns.
        var mod = ModContext.CurrentMod ?? storage.Mod;
        if (mod is not null) SynchronizationContext.SetSynchronizationContext(mod.Scheduler.SynchronizationContext);
        try
        {
            var moved = await As(mod, () => Move(holder, slot, player, 0, MoveOptions.None));
            if (!moved.Succeeded || moved.Changes.Count == 0)
                return EquipResult.Fail(moved.Succeeded ? ItemsStatus.Refused : moved.Status, player, equipSlot, false, slot,
                    $"the stack did not move into the backpack: {(moved.Message.Length > 0 ? moved.Message : moved.Status.ToString())}", false);
            var cell = moved.Changes[0].To;
            var equipped = await As(mod, () => Equip(player, cell, equipSlot));
            if (!equipped.Ok)
            {
                await PutBack(storage, mod, player, holder, moved.Changes);
                return equipped;
            }
            if (storage.Activated.TryGetValue(key, out var before) && before.Cell != cell && before.Item != stack.Item &&
                Items.Read(player)?.Slots.FirstOrDefault(s => s.Slot == before.Cell) is { IsEmpty: false } old && old.Item == before.Item)
            {
                var back = await As(mod, () => Move(player, before.Cell, holder, 0, MoveOptions.Equipped));
                if (!back.Succeeded)
                    Backend.Current.Log(storage.Owner, LogLevel.Info, $"player-storage: {storage.FullKey}: the stack activated before stays in the backpack ({back})");
            }
            storage.Activated[key] = (cell, stack.Item);
            return equipped;
        }
        finally
        {
            storage.Activating.Remove(key);
        }
    }

    // The equip failed after the move: what the move put into the backpack goes back into the storage, and no more (a
    // stack it topped up keeps the player's own items). What cannot go back stays in the backpack, logged.
    private static async Task PutBack(PlayerStorage storage, ModContext? mod, EntityId player, EntityId holder, IReadOnlyList<MoveChange> changes)
    {
        foreach (var c in changes)
        {
            var now = Items.Read(player)?.Slots.FirstOrDefault(s => s.Slot == c.To);
            var n = now is { IsEmpty: false } && now.Item == c.Item ? Math.Min(c.Amount, now.Count) : 0;
            if (n <= 0) continue;
            var back = await As(mod, () => Move(player, c.To, holder, n, MoveOptions.Equipped));
            if (!back.Succeeded)
                Backend.Current.Log(storage.Owner, LogLevel.Info, $"player-storage: {storage.FullKey}: the equip failed and {n} of {c.Item} stay in the backpack ({back})");
        }
    }

    private static Task<MoveResult> Move(EntityId from, SlotId slot, EntityId to, int count, MoveOptions options) =>
        MoveHook is { } hook ? hook(from, slot, to, count, options) : Items.Move(from, slot, to, count, options);

    private static Task<EquipResult> Equip(EntityId player, SlotId from, EquipmentSlot slot) =>
        EquipHook is { } hook ? hook(player, from, slot) : Items.Equip(player, from, slot);

    // a step's request as the mod that asked (an await continuation runs with no mod on the thread)
    private static Task<T> As<T>(ModContext? mod, Func<Task<T>> request)
    {
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = mod;
        try { return request(); }
        finally { ModContext.CurrentMod = outer; }
    }
}

public static unsafe partial class Items
{
    /// <summary>
    /// Registers a storage every player of the server world gets: a quiver, a spellbook, an extra bag. Kiln keeps its items
    /// with the character, paired with the character's own save: across world saves, rejoins, trips to other worlds, the mod
    /// being off (they wait parked) and Kiln being removed (they wait in Kiln's copies). Since 0.2.0-dev.30.7 (mod API
    /// 0.2.16).
    /// </summary>
    /// <param name="key">The storage's name within the mod: lowercase letters, digits, '.', '_' and '-', 32 at most. Its full
    /// key is "&lt;mod id&gt;:&lt;key&gt;". Keep it stable: the saved items belong to it.</param>
    /// <param name="slots">1 to 64. Fewer than saved items: the rest waits parked, never cut.</param>
    /// <param name="filter">What it takes (<see cref="StorageFilter.Any"/> for anything).</param>
    /// <param name="death">What happens at death (<see cref="StorageDeath.Keep"/> by default).</param>
    /// <returns>The storage; ready for a player once Kiln restored it (<see cref="PlayerStorage.IsReady"/>).</returns>
    /// <remarks>Once per key per mod load, any thread, best in <c>OnLoad</c> so it is there before players join (a later
    /// registration restores at once for players already in game). A mod has at most 8 storages, and all mods' storages
    /// together at most 256 slots of page-rounded capacity: each registration counts its slots rounded up to 8 (at most
    /// 32 pages per player), while its logical slot count stays unchanged. When the mod unloads its storages end: their
    /// items are parked for players in game.
    /// Storage never counts for crafting costs. A matching character-inventory witness wins at join; without a match,
    /// the newest copy wins and <see cref="PlayerStorageStatus.Paired"/> is false. Bounded history cannot guarantee an
    /// exact pair after an old backup or offline edit, and a failed durable save can leave an older copy. Game item record
    /// fields travel with the character, but arbitrary mod InstanceData remains world-only.</remarks>
    /// <exception cref="ArgumentException">A bad key or slot count, an unknown death rule, or a filter that takes no item or
    /// too many (more than 1024 by id).</exception>
    /// <exception cref="InvalidOperationException">Outside mod code, the key is registered already, or a limit is reached.</exception>
    /// <exception cref="KilnUnavailableException">The row <c>items.storage</c> is off.</exception>
    [AnyThread, RequiresCapability(PlayerStorage.Capability)]
    public static PlayerStorage RegisterPlayerStorage(string key, int slots, StorageFilter filter, StorageDeath death = StorageDeath.Keep) =>
        PlayerStorageRuntime.Register(key, slots, filter, death);

    /// <summary>Every mod's player storages: what a player has (registered and parked) and the return of parked items. Since
    /// 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
    [RequiresCapability(PlayerStorage.Capability)]
    public static class PlayerStorages
    {
        /// <summary>Every storage key of <paramref name="player"/>: the registered ones (any state) and those whose items wait
        /// parked, sorted by key, with one entry per key. Empty for an unknown player.</summary>
        /// <exception cref="KilnUnavailableException">The row <c>items.storage</c> is off.</exception>
        [TickThreadOnly]
        public static IReadOnlyList<PlayerStorageInfo> Of(EntityId player) => PlayerStorageRuntime.Of(player);

        /// <summary>
        /// Puts every parked item of storage <paramref name="fullKey"/> back into the game: into <paramref name="player"/>'s
        /// backpack (as far as there is room) or a container, rebuilt as Kiln restores storage (gear with its level,
        /// durability, dye, upgrades and gem). What does not fit stays parked. For a mod that is gone for good, or items an
        /// admin releases. Runs in the next sim step; the task completes one step later.
        /// </summary>
        /// <remarks><see cref="MoveResult.Status"/>: <see cref="ItemsStatus.Done"/>, <see cref="ItemsStatus.Partial"/> (some
        /// stay parked), <see cref="ItemsStatus.NoRoom"/>, <see cref="ItemsStatus.NotEnough"/> (nothing parked under that key),
        /// <see cref="ItemsStatus.Invalid"/>, <see cref="ItemsStatus.Unavailable"/>.</remarks>
        /// <exception cref="KilnUnavailableException">The row <c>items.storage</c> is off.</exception>
        [TickThreadOnly]
        public static Task<MoveResult> ReturnOrphans(EntityId player, string fullKey, OrphanTarget target) =>
            PlayerStorageRuntime.ReturnOrphans(player, fullKey, target);
    }
}
