// SPDX-License-Identifier: MIT
// An item's rarity, enhancements and level as values: what Items.GearOf reads and what Items.SetRarity, SetEnhancements
// and SetLevel answer (kiln.items v8, native/core/abi/services/kiln_items.h "gear"). Guide: docs/kiln/services/items.md,
// "Gear: rarity, enhancements, level".
namespace Kiln;

/// <summary>One of an item's five enhancement slots (the game's perk slots: one per rarity, Common's first).</summary>
/// <param name="Index">0 to 4: the slot of the rarity with this value (0 Common .. 4 Legendary).</param>
/// <param name="Available">The item has this slot now (its rarity reaches it): the player can unlock it at the
/// workbench.</param>
/// <param name="Unlocked">The slot is unlocked: its perk acts, and it counts one level.</param>
/// <param name="TypeHasSlot">The item's type has a perk up to this slot (the slot is at or below the type's last filled
/// perk slot; a rare gap below it is not told apart). A rarity whose slot this is false for is refused
/// (<see cref="GearStatus.NoSlot"/>) unless <see cref="GearWriteOptions.AllowEmptySlot"/> is set.</param>
public sealed record PerkSlot(int Index, bool Available, bool Unlocked, bool TypeHasSlot);

/// <summary>
/// One item's rarity, enhancements, level and durability as the server's world has them now (<see cref="Items.GearOf"/>).
/// The game keeps an item's rarity as its number of enhancement slots: a Rare item has three (Common one .. Legendary
/// five), and each unlocked one is one level. The colour, the "LEVEL x/y" line and the damage the game shows are derived
/// from these by the game itself in the next step after a change (<see cref="ShownRarity"/>, <see cref="ShownLevel"/>).
/// </summary>
/// <param name="Item">The item.</param>
/// <param name="Type">Its item type.</param>
/// <param name="Rarity">Its rarity: its enhancement slots - 1 (the game's itemRarityOf), or the type's rarity when the
/// item has no enhancement slots.</param>
/// <param name="TypeRarity">The rarity its type names (<c>ItemInfo.rarity</c>).</param>
/// <param name="MaxRarity">The highest rarity its type has a perk slot for: the most <see cref="Items.SetRarity"/> gives
/// without <see cref="GearWriteOptions.AllowEmptySlot"/>. <see cref="TypeRarity"/> when the type has no perk slot.</param>
/// <param name="Level">Its level, the unlocked enhancements included (the game's Level attribute; the shown level when it
/// has none).</param>
/// <param name="BaseLevel"><see cref="Level"/> without the unlocked enhancements: the level it was made at, inside the
/// type's level range.</param>
/// <param name="Enhancements">How many enhancement slots it has (rarity + 1; 0 when it has none).</param>
/// <param name="Unlocked">How many of them are unlocked.</param>
/// <param name="MaxLevel">The level it reaches with every slot unlocked (the game's "LEVEL x/max").</param>
/// <param name="HasSocket">It has a gem socket (the game's HasWeaponGemSlot item state).</param>
/// <param name="Durability">Its durability now; null when it has none.</param>
/// <param name="DurabilityMax">Its max durability; null when it has none. After a rarity or level change with
/// <see cref="DurabilityRule.KeepRatio"/> the game computes the new max in the next step.</param>
/// <param name="Perks">The five enhancement slots.</param>
public sealed record GearState(
    ItemInstanceRef Item,
    ItemId Type,
    ItemRarity Rarity,
    ItemRarity TypeRarity,
    ItemRarity MaxRarity,
    int Level,
    int BaseLevel,
    int Enhancements,
    int Unlocked,
    int MaxLevel,
    bool HasSocket,
    int? Durability,
    int? DurabilityMax,
    IReadOnlyList<PerkSlot> Perks)
{
    /// <summary>The item has enhancement slots at all (the game's PerkContainerNew): only then can its rarity change.</summary>
    public bool HasEnhancementSlots { get; init; }

    /// <summary>The rarity the game derived and sends to players (<c>ItemState.itemRarityUi</c>): it follows a change in the
    /// next step.</summary>
    public ItemRarity ShownRarity { get; init; }

    /// <summary>The level the game derived and sends to players (<c>ItemState.itemLevelUi</c>): it follows a change in the
    /// next step.</summary>
    public int ShownLevel { get; init; }

    /// <summary>The lowest level of the type's level range (0: none).</summary>
    public int TypeLevelMin { get; init; }

    /// <summary>The highest level of the type's level range (0: none).</summary>
    public int TypeLevelMax { get; init; }

    /// <summary>A player wears or holds it in an equipment slot.</summary>
    public bool IsEquipped { get; init; }

    /// <summary>One line for a log: <c>"Rare 1/3 level 6/8"</c>.</summary>
    public override string ToString() =>
        $"{Item}: {Rarity} {Unlocked}/{Enhancements} level {Level}/{MaxLevel}" +
        (Durability is { } d ? $" durability {d}/{DurabilityMax}" : "") + (IsEquipped ? " (equipped)" : "");
}

/// <summary>What a gear write did (<see cref="GearResult.Status"/>); the values are kiln.items' own.</summary>
public enum GearStatus
{
    /// <summary>Written (a dry run: would be); also when nothing needed a change (<see cref="GearResult.Changed"/> none).</summary>
    Done = 0,

    /// <summary>The item is gone, or its id names another item now: nothing changed.</summary>
    Stale = 1,

    /// <summary>The item has no enhancement slots (its type has none), or no level for a level change.</summary>
    NotGear = 2,

    /// <summary>The rarity asked for needs an enhancement slot the item's type has no perk for (tools, a few rings and
    /// armour pieces); <see cref="GearWriteOptions.AllowEmptySlot"/> gives it anyway.</summary>
    NoSlot = 3,

    /// <summary>More enhancements unlocked than the item has slots, or a level whose base is outside the type's level
    /// range (the game would set it back when the world loads).</summary>
    OutOfRange = 4,

    /// <summary><see cref="GearWriteOptions.RefuseEquipped"/> and the item is equipped.</summary>
    Equipped = 5,

    /// <summary>Gear writes went off after the call (a fault): nothing changed.</summary>
    Unavailable = 6,

    /// <summary>Not run: the server world closed, or did not step for 10 seconds.</summary>
    Dropped = 7,

    /// <summary>Refused: a component of the item is not laid out as Kiln expects, or 1024 writes were waiting.</summary>
    Refused = 8,
}

/// <summary>What a gear write changed (<see cref="GearResult.Changed"/>).</summary>
[Flags]
public enum GearChanges
{
    /// <summary>Nothing.</summary>
    None = 0,

    /// <summary>The enhancement slots or the unlocked ones (so the rarity, or the unlocked count).</summary>
    Enhancements = 1,

    /// <summary>The level.</summary>
    Level = 2,

    /// <summary>The current durability (<see cref="DurabilityRule.Repair"/>).</summary>
    Durability = 4,

    /// <summary>The game was told to compute max durability again (<see cref="DurabilityRule.KeepRatio"/>,
    /// <see cref="DurabilityRule.Repair"/>); it does so in the next step.</summary>
    DurabilityRefresh = 8,
}

/// <summary>What happens to an item's durability when its rarity, enhancements or level change.</summary>
public enum DurabilityRule
{
    /// <summary>The game computes max durability again at the new rarity and level in the next step, and the item keeps
    /// its share of it (half worn stays half worn). The default.</summary>
    KeepRatio = 0,

    /// <summary>Max durability stays as it is (the game computes it only once per item).</summary>
    Keep = 1,

    /// <summary>As <see cref="KeepRatio"/>, and the item is fully repaired.</summary>
    Repair = 2,
}

/// <summary>Options of <see cref="Items.SetRarity"/>, <see cref="Items.SetEnhancements"/> and <see cref="Items.SetLevel"/>.</summary>
public sealed record GearWriteOptions
{
    /// <summary>The defaults: durability <see cref="DurabilityRule.KeepRatio"/>, no empty slot, equipped items allowed.</summary>
    public static GearWriteOptions Default { get; } = new();

    /// <summary>What happens to durability (default <see cref="DurabilityRule.KeepRatio"/>).</summary>
    public DurabilityRule Durability { get; init; } = DurabilityRule.KeepRatio;

    /// <summary>Give a rarity whose enhancement slot the item's type has no perk for: the slot exists and can be unlocked
    /// (a level), but no perk acts in it. Default false: such a write ends <see cref="GearStatus.NoSlot"/>.</summary>
    public bool AllowEmptySlot { get; init; }

    /// <summary>Refuse (<see cref="GearStatus.Equipped"/>) when a player wears or holds the item. Default false: an equipped
    /// item changes in place too.</summary>
    public bool RefuseEquipped { get; init; }

    /// <summary>Say what would change, change nothing.</summary>
    public bool DryRun { get; init; }
}

/// <summary>What wrote a gear change (<see cref="GearEvent.Source"/>).</summary>
public enum GearChangeSource
{
    /// <summary>A mod's <see cref="Items.SetRarity"/>, <see cref="Items.SetEnhancements"/> or <see cref="Items.SetLevel"/>.</summary>
    Code = 0,

    /// <summary>An upgrade action (a rarity upgrade at a station, or its code path).</summary>
    Upgrade = 1,

    /// <summary>The game: a player's unlock at the workbench, or any change Kiln did not make, seen by Kiln's read of the
    /// players' items.</summary>
    Game = 2,
}

/// <summary>What <see cref="Items.SetRarity"/>, <see cref="Items.SetEnhancements"/> or <see cref="Items.SetLevel"/> did.
/// <see cref="Ok"/> when the write was made (a dry run: would be), also when nothing needed a change; otherwise
/// <see cref="Reason"/> says why and nothing changed.</summary>
/// <param name="Status">The outcome.</param>
/// <param name="Item">The item.</param>
/// <param name="Before">The item before the write; null when it never ran or the item was not found.</param>
/// <param name="After">The item after it (a dry run: what it would be; a refusal: as before); null as
/// <see cref="Before"/>. Its <see cref="GearState.ShownRarity"/>, <see cref="GearState.ShownLevel"/> and a refreshed max
/// durability follow in the game's next step: <see cref="Items.GearOf"/> then shows them.</param>
/// <param name="Changed">What changed (a dry run: would change).</param>
/// <param name="Message">Why, for anything but <see cref="GearStatus.Done"/>.</param>
/// <param name="Steps">Sim steps between the call and the write.</param>
/// <param name="DryRun">Run with <see cref="GearWriteOptions.DryRun"/>: nothing changed.</param>
public sealed record GearResult(
    GearStatus Status,
    ItemInstanceRef Item,
    GearState? Before,
    GearState? After,
    GearChanges Changed,
    string Message,
    int Steps,
    bool DryRun) : IKilnResult
{
    /// <summary>True when the write was made (a dry run: would be).</summary>
    public bool Ok => Status == GearStatus.Done;

    /// <summary>Why it did not happen; null when <see cref="Ok"/>.</summary>
    public string? Reason => Ok ? null : Message.Length > 0 ? Message : Status.ToString();

    /// <summary>The outcome in one line, for a log.</summary>
    public override string ToString() =>
        $"gear {Status} {Item}: {Before?.Rarity.ToString() ?? "?"} {Before?.Unlocked}/{Before?.Enhancements} level {Before?.Level} -> " +
        $"{After?.Rarity.ToString() ?? "?"} {After?.Unlocked}/{After?.Enhancements} level {After?.Level}" +
        (Changed == GearChanges.None ? "" : $" ({Changed})") + (DryRun ? " (dry run)" : "") + (Message.Length > 0 ? $": {Message}" : "");

    internal static GearResult Fail(GearStatus status, ItemInstanceRef item, string message, bool dryRun) =>
        new(status, item, null, null, GearChanges.None, message, 0, dryRun);
}
