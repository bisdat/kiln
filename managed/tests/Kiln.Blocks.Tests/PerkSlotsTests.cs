// SPDX-License-Identifier: MIT
// Enhancement slots of a made weapon (dev.30 game test, equip.c10): a class with a perk source (sword, mace, staff) fills every slot up to
// legendary's, whatever the item's own rarity, so a rarity roll at creation, an anvil or a forge can raise it. Before, a Common sword got
// slot 0 only and its rolls could only pick Common. A class without a perk source, and armour, keep the slots they had.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class PerkSlotsTests
{
    private const string Mod = "perk-slots-test";

    private static JsonObject Lower(IBlock b) => Compiler.Lower(new ModBlocks(Mod, 1) { ManifestClients = "mod" }, "blade", b).Low.Json;

    private static string?[]? Perks(JsonObject j) => j["itemParts"]?["perks"]?.AsArray().Select(n => (string?)n).ToArray();

    [Theory]
    [InlineData("sword", "Weapon_T1_1H_Sword_01")]
    [InlineData("staff", "Weapon_T1_2H_Staff_01")]
    [InlineData("mace", "Weapon_T1_1H_MaceEpic_01")]
    public void A_common_weapon_of_a_class_with_a_perk_source_has_every_slot(string key, string source)
    {
        var cls = key switch { "sword" => WeaponClass.Sword, "staff" => WeaponClass.Staff, _ => WeaponClass.Mace };
        var j = Lower(new WeaponBlock { Name = "Blade", Class = cls, Stats = new WeaponStats { Level = 5, Power = 25 } });
        var perks = Perks(j);
        Assert.NotNull(perks);
        Assert.Equal(5, perks!.Length);
        // slot 0 is the starter's own
        Assert.Null(perks[0]);
        for (var i = 1; i < 5; i++) Assert.Equal("item:" + source, perks[i]);
        // still made and shown as Common: the game reads the slot of the rarity it has
        Assert.Equal("Common", (string?)j["items"]![0]!["fields"]!["rarity"]);
    }

    [Fact]
    public void The_rarity_anvil_samples_blade_has_every_slot()
    {
        var j = Lower(new WeaponBlock { Name = "Anvil Blade", Class = WeaponClass.Sword, Stats = new WeaponStats { Level = 5, Power = 25 } });
        Assert.Equal([null, "item:Weapon_T1_1H_Sword_01", "item:Weapon_T1_1H_Sword_01", "item:Weapon_T1_1H_Sword_01", "item:Weapon_T1_1H_Sword_01"], Perks(j));
    }

    [Fact]
    public void A_rare_sword_fills_the_slots_above_its_rarity_too()
    {
        var j = Lower(new WeaponBlock { Name = "Blade", Class = WeaponClass.Sword, Stats = new WeaponStats { Rarity = Rarity.Rare } });
        Assert.Equal([null, "item:Weapon_T1_1H_Sword_01", "item:Weapon_T1_1H_Sword_01", "item:Weapon_T1_1H_Sword_01", "item:Weapon_T1_1H_Sword_01"], Perks(j));
        Assert.Equal("Rare", (string?)j["items"]![0]!["fields"]!["rarity"]);
    }

    [Fact]
    public void A_slot_above_the_items_rarity_left_empty_on_purpose_stays_empty()
    {
        var j = Lower(new WeaponBlock { Name = "Blade", Class = WeaponClass.Sword, Perks = [(string?)null, (string?)null, (string?)null, "none"] });
        Assert.Equal([null, "item:Weapon_T1_1H_Sword_01", "item:Weapon_T1_1H_Sword_01", "none", "item:Weapon_T1_1H_Sword_01"], Perks(j));
    }

    [Fact]
    public void A_slot_the_items_rarity_needs_still_cannot_be_empty()
    {
        var e = Assert.Throws<BlockError>(() => Lower(new WeaponBlock { Name = "Blade", Class = WeaponClass.Sword, Stats = new WeaponStats { Rarity = Rarity.Uncommon }, Perks = [(string?)null, "none"] }));
        Assert.Contains("can't be empty", e.Message);
    }

    [Fact]
    public void A_weapon_of_a_class_without_a_perk_source_is_unchanged()
    {
        // the axe's starter has all five slots: nothing to fill
        Assert.Null(Perks(Lower(new WeaponBlock { Name = "Axe", Class = WeaponClass.Axe })));
        Assert.Null(Perks(Lower(new WeaponBlock { Name = "Axe", Class = WeaponClass.Axe, Stats = new WeaponStats { Rarity = Rarity.Legendary } })));
    }

    [Fact]
    public void Armour_is_unchanged()
    {
        Assert.Null(Perks(Lower(new ArmorBlock { Name = "Chest", Slot = ArmorSlot.Chest })));
    }
}
