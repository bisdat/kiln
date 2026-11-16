// SPDX-License-Identifier: MIT
namespace Kiln;

// The gem system's shared types (gem wave, /mnt/Dongus/kiln-wave/gems/DESIGN.md). A gem is an item of category
// WeaponGem (15) with its own item entity and Level (1..105); its effect is the Impact program in its ItemInfo's
// equipment.program with the configs in impactValues, scaled by the gem's level. docs/kiln/services/gems.md.

/// <summary>A gem item definition (category WeaponGem) from the catalog: the game's own gems and any mod's.</summary>
public sealed record GemInfo(ItemId Item, string Name, string? DisplayName, ItemRarity Rarity, int LevelMin, int LevelMax,
    Guid Program, IReadOnlyDictionary<uint, GemConfigValue> Configs, bool FromMod);

/// <summary>One config of a gem's impact program. <see cref="Id"/> is the config's id as kiln.gamedata writes it
/// (<c>impactValues.0x5e34f09b</c>); <see cref="Kind"/> the config type (for example <c>ScaledSint32</c>, <c>BuffType</c>);
/// <see cref="Value"/> its base value and <see cref="ScaleFactor"/> the level scaling of a scaled config.</summary>
public sealed record GemConfigValue(uint Id, string? Alias, string Kind, double Value, double? ScaleFactor);

/// <summary>A gem in a socket: the carrier item, the gem item and its level.</summary>
public readonly record struct SocketedGem(ItemInstanceRef Carrier, ItemId Gem, int Level);

/// <summary>The game's equipment impact slots: a socketed gem runs from the item in any of them.</summary>
public enum GemImpactSlot : byte
{
    Held = 0, Shield = 1, Head = 2, Chest = 3, Hands = 4, Legs = 5, Feet = 6, Ring1 = 7, Ring2 = 8, Ammo = 9, Spell = 10,
}

/// <summary>A gem whose effect runs for a player now: the game spawned its impact entity.</summary>
public sealed record EquippedGem(EntityId Player, GemImpactSlot Slot, SocketedGem Socket, EntityId Impact);
