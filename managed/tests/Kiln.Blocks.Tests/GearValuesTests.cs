// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>Typed gear values (Content.Gear, GearStats) over a fake item, and the formulas they share with item blocks (GearMath):
/// the tooltip's numbers written read back as written at every level, ops on the right fields, refusals with their reasons.</summary>
public sealed class GearValuesTests
{
    private sealed class FakeItem(string name, Dictionary<string, double> game) : IGearFields
    {
        public readonly Dictionary<string, double> Layers = new();
        public readonly Dictionary<string, string> Names = new();
        public string Name => name;
        public double? Current(string path) => Layers.TryGetValue(path, out var v) ? v : game.TryGetValue(path, out var g) ? g : null;
        public double? Original(string path) => game.TryGetValue(path, out var g) ? g : null;
        public void Set(string path, double value) { Require(path); Layers[path] = (float)value; }   // float fields, as the game's
        public void SetInt(string path, long value) { Require(path); Layers[path] = value; }
        public void SetBool(string path, bool value) { Require(path); Layers[path] = value ? 1 : 0; }
        public void SetName(string path, string n) { Require(path); Names[path] = n; Layers[path] = Enum.Parse<ItemRarity>(n) is var r ? (double)(int)r : 0; }
        public bool Reset(string path) { Names.Remove(path); return Layers.Remove(path); }
        private void Require(string path) { if (!game.ContainsKey(path)) throw new KeyNotFoundException($"Items has no field {path}"); }
    }

    private static Dictionary<string, double> Sword() => new()
    {
        ["itemLevelRange.minLevel"] = 5, ["itemLevelRange.maxLevel"] = 5, ["damageSetup.isSet"] = 1, ["damageSetup.dmgMod"] = 1,
        ["damageSetup.speedMod"] = 1, ["damageSetup.distribution.physicalCutDamage"] = 1, ["damageSetup.distribution.magicalFireDamage"] = 0,
        ["damageSetup.distribution.woodDamage"] = 0, ["armorSetup.isSet"] = 0, ["armorSetup.distribution.physical"] = 0, ["blockSetup.quality"] = 0.5,
        ["rarity"] = 0, ["generateRarity"] = 0, ["gemSlotOverride"] = 0, ["maxStackSize"] = 1,
    };

    private static Dictionary<string, double> Chest() => new()
    {
        ["itemLevelRange.minLevel"] = 10, ["itemLevelRange.maxLevel"] = 10, ["damageSetup.isSet"] = 0, ["damageSetup.dmgMod"] = 0,
        ["armorSetup.isSet"] = 1, ["armorSetup.distribution.physical"] = 0.18, ["armorSetup.distribution.magical"] = 0.10, ["rarity"] = 2,
    };

    [Fact]
    public void ItemBlocksAndGearValuesShareOneCurve()
    {
        foreach (var level in new[] { 1, 5, 10, 20, 37, 55 })
        {
            Assert.Equal(Compiler.BasePower(level), GearMath.BasePower(level));
            Assert.Equal(Compiler.DmgModFor(30, level), GearMath.DmgModFor(30, level));
            Assert.Equal(Compiler.ArmorShare(30, level), GearMath.ArmorShare(30, level));
        }
        // the same values as the native "gear" section's test (native/services/content/tests/gear_test.cpp)
        Assert.Equal([11, 17, 25, 40, 92], new[] { 1, 5, 10, 20, 55 }.Select(GearMath.BasePower));
    }

    [Fact]
    public void EveryPowerAndArmourReadsBackAsWritten()
    {
        for (var level = 1; level <= 55; level++)
        {
            foreach (var speed in new[] { 0.8, 1.0, 1.25, 1.6 })
                for (var p = 0; p <= 600; p++)
                    Assert.Equal(p, GearMath.PowerOf((float)GearMath.DmgModFor(p, level, speed), (float)speed, level));
            for (var a = 0; a <= 400; a++) Assert.Equal(a, GearMath.ArmorOf((float)GearMath.ArmorShare(a, level), level));
        }
    }

    [Fact]
    public void PowerIsTheTooltipsNumberAtTheItemsLevel()
    {
        var item = new FakeItem("Weapon_T1_1H_Sword_Craft", Sword());
        var g = new GearStats(item);
        Assert.True(g.HasDamage);
        Assert.False(g.HasArmor);
        Assert.Equal(17, g.Power);            // int(int(10 * 0.15 * 5 + 10) * 1 * 1)
        g.Power = 30;
        Assert.Equal(30, g.Power);
        Assert.Equal(30.5 / 17, item.Layers["damageSetup.dmgMod"], 5);
        g.SetLevels(10, 12);
        Assert.Equal((10, 12), (g.Level, g.MaxLevel));
        Assert.Equal(44, g.Power);            // the game's curve took the damage to level 10: int(25 * 30.5 / 17)
        g.Power = 30;
        Assert.Equal(30, g.Power);
        Assert.Equal(30, g.PowerAt(10));
        Assert.True(g.PowerAt(12) > 30);
        g.Speed = 1.25;
        g.Power = 30;
        Assert.Equal(30, g.Power);
        Assert.Equal(1.25, g.Speed);
    }

    [Fact]
    public void DamageArmourAndTheSimpleValuesGoToTheirFields()
    {
        var item = new FakeItem("Weapon_T1_1H_Sword_Craft", Sword());
        var g = new GearStats(item);
        g.SetDamage(GearDamageKind.Cut, 0.7);
        g.SetDamage(GearDamageKind.Fire, 0.3);
        g.SetDamage(GearDamageKind.Wood, 2.6);
        Assert.Equal(0.7, g.Damage(GearDamageKind.Cut), 5);
        Assert.Equal(0.3, item.Layers["damageSetup.distribution.magicalFireDamage"], 5);
        Assert.Equal(3, item.Layers["damageSetup.distribution.woodDamage"]);
        g.Block = 0.75;
        g.Rarity = ItemRarity.Epic;
        g.Rolled = true;
        g.Socket = true;
        g.MaxStack = 1;
        Assert.Equal("Epic", item.Names["rarity"]);
        Assert.Equal(ItemRarity.Epic, g.Rarity);
        Assert.True(g.Rolled && g.Socket);
        Assert.Equal(0.75, g.Block, 5);

        var chest = new FakeItem("Armor_T2_Warrior_Tank_Chest", Chest());
        var a = new GearStats(chest);
        Assert.Equal(31, a.Armor(GearArmorKind.Physical));      // round(173 * 0.18)
        Assert.Equal(17, a.Armor(GearArmorKind.Magical));
        a.SetArmor(GearArmorKind.Physical, 45);
        Assert.Equal(45, a.Armor(GearArmorKind.Physical));
        Assert.Equal(45 / 173.0, chest.Layers["armorSetup.distribution.physical"], 5);
        Assert.Equal(0, a.Power);
    }

    [Fact]
    public void WrongValuesAreRefusedWithTheirReason()
    {
        var g = new GearStats(new FakeItem("Weapon_T1_1H_Sword_Craft", Sword()));
        Assert.Contains("not within the game's item levels 1..55", Assert.Throws<ArgumentOutOfRangeException>(() => g.SetLevels(0, 60)).Message);
        Assert.Throws<ArgumentOutOfRangeException>(() => g.SetLevels(12, 10));
        Assert.Throws<ArgumentOutOfRangeException>(() => g.Power = -1);
        Assert.Throws<ArgumentOutOfRangeException>(() => g.Speed = 0);
        Assert.Throws<ArgumentOutOfRangeException>(() => g.Block = 2);
        Assert.Throws<ArgumentOutOfRangeException>(() => g.MaxStack = 70000);
        Assert.Throws<ArgumentOutOfRangeException>(() => g.Rarity = (ItemRarity)5);
        Assert.Throws<ArgumentOutOfRangeException>(() => g.SetDamage(GearDamageKind.Cut, -0.1));
        Assert.Contains("no armour values", Assert.Throws<InvalidOperationException>(() => g.SetArmor(GearArmorKind.Physical, 10)).Message);
        var chest = new GearStats(new FakeItem("Armor_T2_Warrior_Tank_Chest", Chest()));
        Assert.Contains("deals no damage of its own", Assert.Throws<InvalidOperationException>(() => chest.Power = 30).Message);
        Assert.Throws<InvalidOperationException>(() => chest.SetDamage(GearDamageKind.Fire, 1));
    }

    [Fact]
    public void ResetTakesOnlyThisModsLayersOff()
    {
        var item = new FakeItem("Weapon_T1_1H_Sword_Craft", Sword());
        var g = new GearStats(item);
        g.Power = 30;
        g.SetLevels(20);
        g.SetDamage(GearDamageKind.Fire, 1);
        Assert.Equal(4, g.Reset());
        Assert.Empty(item.Layers);
        Assert.Equal(17, g.Power);
        Assert.Equal(0, g.Reset());
    }

    [Fact]
    public void KindsNameTheGamesFields()
    {
        Assert.Equal("damageSetup.distribution.magicalFogDamage", GearStats.DamagePath(GearDamageKind.Shroud));
        Assert.Equal("armorSetup.distribution.fog", GearStats.ArmorPath(GearArmorKind.Shroud));
        Assert.All(Enum.GetValues<GearDamageKind>(), k => Assert.StartsWith("damageSetup.distribution.", GearStats.DamagePath(k)));
        Assert.All(Enum.GetValues<GearArmorKind>(), k => Assert.StartsWith("armorSetup.distribution.", GearStats.ArmorPath(k)));
    }
}
