// SPDX-License-Identifier: MIT
namespace Kiln;

// The gem system's shared types (gem wave, /mnt/Dongus/kiln-wave/gems/DESIGN.md). A gem is an item of category
// WeaponGem (15) with its own item entity and Level (1..105); its effect is the Impact program in its ItemInfo's
// equipment.program with the configs in impactValues, scaled by the gem's level. docs/kiln/services/gems.md.

/// <summary>A gem item definition (category WeaponGem) from the catalog: the game's own gems and any mod's.
/// <c>DisplayName</c> is a game gem's English name, or the name a mod's definition gives its gem (<c>displayName</c>,
/// else its English text); null without one.</summary>
/// <param name="Item">The gem's item id.</param>
/// <param name="Name">The item's name.</param>
/// <param name="DisplayName">The English display name, or the name a mod's definition gave; null without one.</param>
/// <param name="Rarity">The item's rarity.</param>
/// <param name="LevelMin">The level a new gem has (the low end of the item's level range).</param>
/// <param name="LevelMax">The highest level the gem can reach (the high end of the item's level range).</param>
/// <param name="Program">The Impact program the gem runs; empty when it is not known.</param>
/// <param name="Configs">The program's config values by config id (the gem's <c>impactValues</c>).</param>
/// <param name="FromMod">True for a gem a mod defined: a mod's own item, or a clone of a game item.</param>
public sealed record GemInfo(ItemId Item, string Name, string? DisplayName, ItemRarity Rarity, int LevelMin, int LevelMax,
    Guid Program, IReadOnlyDictionary<uint, GemConfigValue> Configs, bool FromMod);

/// <summary>One config of a gem's impact program. <see cref="Id"/> is the config's id as kiln.gamedata writes it
/// (<c>impactValues.0x5e34f09b</c>); <see cref="Kind"/> the config type (for example <c>ScaledSint32</c>, <c>BuffType</c>);
/// <see cref="Value"/> its base value and <see cref="ScaleFactor"/> the level scaling of a scaled config.</summary>
/// <param name="Id">The config's id.</param>
/// <param name="Alias">The config's alias in the shared vocabulary (<see cref="ImpactConfigIds"/>); null when it has
/// none.</param>
/// <param name="Kind">The config type, for example <c>ScaledSint32</c> or <c>BuffType</c>.</param>
/// <param name="Value">The base value; 0 when the value is not a number (see <see cref="Text"/>).</param>
/// <param name="ScaleFactor">The level scaling of a scaled config; null for an unscaled one.</param>
public sealed record GemConfigValue(uint Id, string? Alias, string Kind, double Value, double? ScaleFactor)
{
    /// <summary>The value as kiln.gamedata writes it when it is not a number: a GUID (a buff, a template), an enum name
    /// (<c>Big</c>), <c>none</c>; then <see cref="Value"/> is 0. Null for a number.</summary>
    public string? Text { get; init; }
}

/// <summary>A gem in a socket: the carrier item, the gem item and its level.</summary>
/// <param name="Carrier">The item that holds the socket.</param>
/// <param name="Gem">The gem item in the socket.</param>
/// <param name="Level">The gem's level.</param>
public readonly record struct SocketedGem(ItemInstanceRef Carrier, ItemId Gem, int Level);

/// <summary>The game's equipment impact slots: a socketed gem runs from the item in any of them.</summary>
public enum GemImpactSlot : byte
{
    /// <summary>The item held in the hand: a weapon or a tool.</summary>
    Held = 0,
    /// <summary>The shield.</summary>
    Shield = 1,
    /// <summary>The head armour.</summary>
    Head = 2,
    /// <summary>The chest armour.</summary>
    Chest = 3,
    /// <summary>The hands armour.</summary>
    Hands = 4,
    /// <summary>The legs armour.</summary>
    Legs = 5,
    /// <summary>The feet armour.</summary>
    Feet = 6,
    /// <summary>The first ring slot.</summary>
    Ring1 = 7,
    /// <summary>The second ring slot.</summary>
    Ring2 = 8,
    /// <summary>The ammunition slot.</summary>
    Ammo = 9,
    /// <summary>The spell slot.</summary>
    Spell = 10,
}

/// <summary>A gem whose effect runs for a player now: the game spawned its impact entity.</summary>
/// <param name="Player">The player.</param>
/// <param name="Slot">The equipment impact slot the gem's item is in.</param>
/// <param name="Socket">The gem in its socket, with the item that carries it.</param>
/// <param name="Impact">The gem's impact entity the game spawned.</param>
public sealed record EquippedGem(EntityId Player, GemImpactSlot Slot, SocketedGem Socket, EntityId Impact);
