// SPDX-License-Identifier: MIT
// The game's formulas behind the numbers a gear tooltip shows, shared by Kiln.Content's gear values (GearStats, Content.Gear) and
// Kiln.Blocks' item blocks (Compiler.DmgModFor, ArmorShare), so that "power 30" means the same everywhere. kiln.content's native
// "gear" pack section has the same numbers (native/services/content/gear.h); both are tested against the same round trips.
namespace Kiln;

internal static class GearMath
{
    // The game's damage power: int(int(seed * creep * level + seed) * dmgMod * speedMod), keen::BalancingTable +0 genericWeaponSeed,
    // +0x28 powerCreepWeapon (server 0x91e9d0). Checked against the running game when content is ready (Kiln.Blocks' BlockRuntime).
    // Fields, not constants: the float math runs as the game's does, not folded at compile time.
    internal static readonly uint Seed = 10;
    internal static readonly float Creep = 0.15f;

    // The game's armour: round((gradient * level + offset) * distribution[kind]) (client 0xe6dfe0, keen::BalancingTable +0x2a0
    // armorSetup). Checked at ready (Kiln.Blocks' ItemChecks).
    internal static readonly float ArmorOffset = 23, ArmorGradient = 15;

    /// <summary>The game's item levels (Kiln.Blocks' CheckLevel).</summary>
    internal const int MinLevel = 1, MaxLevel = 55;

    /// <summary>The base of the damage power at a level (float, as the game).</summary>
    public static int BasePower(int level) => (int)(long)(Seed * Creep * level + Seed);

    /// <summary>The tooltip's damage: int(BasePower(level) * dmgMod * speedMod).</summary>
    public static int PowerOf(double dmgMod, double speedMod, int level)
    {
        var x = BasePower(level) * (float)dmgMod * (float)speedMod;
        return x <= 0 ? 0 : (int)x;
    }

    /// <summary>The dmgMod that makes a level's power (with speedMod 1) this number, as the tooltip shows it.</summary>
    public static double DmgModFor(int power, int level) => (power + 0.5) / BasePower(level);

    /// <summary>The dmgMod that makes the tooltip show this power at this level and speed.</summary>
    public static double DmgModFor(int power, int level, double speedMod) => (power + 0.5) / (BasePower(level) * speedMod);

    /// <summary>The tooltip's armour of one kind at a level.</summary>
    public static int ArmorOf(double share, int level) =>
        (int)MathF.Round((ArmorGradient * level + ArmorOffset) * (float)share, MidpointRounding.AwayFromZero);

    /// <summary>The share of one armour kind that makes the tooltip show this armour at this level.</summary>
    public static double ArmorShare(int armor, int level) => armor / (ArmorGradient * level + ArmorOffset);
}
