// SPDX-License-Identifier: MIT
// Rarity upgrades as a block (package P3 of the equipment wave): RarityUpgradeBlock checked by field, its JSON form, its
// lowering, the rule it registers in Kiln.Items (names read in its mod) and Items.UpgradeRarity by the block's name on the
// test kit. The upgrade itself: managed/tests/Kiln.Items.Tests/UpgradeTests.cs and native/services/items/tests/gear_test.cpp.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class RarityUpgradeBlockTests
{
    private static RarityUpgradeBlock Standard() => new()
    {
        Items = ["Weapon_*"],
        Steps =
        {
            [Rarity.Uncommon] = new UpgradeStep { Cost = { ["Material_T1_Metal_Scraps"] = 10 } },
            [Rarity.Rare] = new UpgradeStep { Cost = { ["ember-shard"] = 3 }, Chance = 0.75, OnFail = UpgradeFail.Downgrade, LevelScale = 0.1, Unlock = true },
        },
    };

    private static JsonObject Lower(RarityUpgradeBlock b) =>
        Compiler.Lower(new ModBlocks("upgrades-test", 1) { ManifestClients = "none" }, "anvil", b).Low.Json;

    private static string Field(RarityUpgradeBlock b) => Assert.Throws<BlockError>(() => Lower(b)).Field ?? "";

    [Fact]
    public void A_rule_lowers_to_its_id_items_and_steps()
    {
        var r = Lower(Standard())["rarityUpgrade"]!;
        Assert.Equal("upgrades-test:anvil", (string)r["id"]!);
        Assert.Equal(["Weapon_*"], r["items"]!.AsArray().Select(n => (string)n!));
        Assert.Equal("Legendary", (string)r["max"]!);
        Assert.Equal(10, (int)r["steps"]!["Uncommon"]!["cost"]!["Material_T1_Metal_Scraps"]!);
        Assert.Equal(0.75, (double)r["steps"]!["Rare"]!["chance"]!);
        Assert.Equal("Downgrade", (string)r["steps"]!["Rare"]!["onFail"]!);
        Assert.True((bool)r["steps"]!["Rare"]!["unlock"]!);
        Assert.Equal("KeepRatio", (string)r["durability"]!);
    }

    [Fact]
    public void A_wrong_rule_is_refused_by_field()
    {
        Assert.Equal("steps", Field(new RarityUpgradeBlock()));
        Assert.Equal("steps", Field(new RarityUpgradeBlock { Steps = { [Rarity.Common] = new UpgradeStep() } }));
        Assert.Equal("max", Field(Standard() with { Max = Rarity.Common }));
        Assert.Equal("steps.rare", Field(Standard() with { Max = Rarity.Uncommon }));
        Assert.Equal("items[0]", Field(Standard() with { Items = [" "] }));
        Assert.Equal("steps.uncommon.chance", Field(new RarityUpgradeBlock { Steps = { [Rarity.Uncommon] = new UpgradeStep { Chance = 2 } } }));
        Assert.Equal("steps.uncommon.levelScale", Field(new RarityUpgradeBlock { Steps = { [Rarity.Uncommon] = new UpgradeStep { LevelScale = -0.5 } } }));
        Assert.Equal("steps.uncommon.cost.Shard", Field(new RarityUpgradeBlock { Steps = { [Rarity.Uncommon] = new UpgradeStep { Cost = { ["Shard"] = 0 } } } }));
        Assert.Equal("durability", Field(Standard() with { Durability = (DurabilityRule)7 }));
        Assert.Equal("socket", Field(Standard() with { Socket = (SocketRule)7 }));
    }

    [Fact]
    public void The_json_form_is_the_record()
    {
        var b = BlockJson.Parse<RarityUpgradeBlock>("""
            { "items": ["Weapon_*", "my-mod:*"], "max": "epic",
              "steps": { "uncommon": { "cost": { "Material_T1_Metal_Scraps": 10 } },
                         "epic": { "cost": { "my-mod:ember-shard": 3 }, "chance": 0.75, "onFail": "keepItemKeepCost", "levelScale": 0.1, "unlock": true } },
              "durability": "repair", "socket": "gameChance", "allowEmptySlot": true }
            """);
        Assert.Equal(["Weapon_*", "my-mod:*"], b.Items!.Select(i => i.Value));
        Assert.Equal(Rarity.Epic, b.Max);
        Assert.Equal(10, b.Steps[Rarity.Uncommon].Cost["Material_T1_Metal_Scraps"]);
        Assert.Equal(UpgradeStep.DefaultChance, b.Steps[Rarity.Uncommon].Chance);
        Assert.Equal(UpgradeFail.KeepItemLoseCost, b.Steps[Rarity.Uncommon].OnFail);
        var epic = b.Steps[Rarity.Epic];
        Assert.Equal((0.75, UpgradeFail.KeepItemKeepCost, 0.1, true), (epic.Chance, epic.OnFail, epic.LevelScale, epic.Unlock));
        Assert.Equal(3, epic.Cost["my-mod:ember-shard"]);
        Assert.Equal((DurabilityRule.Repair, SocketRule.GameChance, true), (b.Durability, b.Socket, b.AllowEmptySlot));
        Assert.ThrowsAny<System.Text.Json.JsonException>(() => BlockJson.Parse<RarityUpgradeBlock>("""{ "steps": { "rare": { "price": 3 } } }"""));
    }

    [Fact]
    public void The_rule_reads_the_blocks_names_in_its_mod()
    {
        var rule = RarityUpgradeRuntime.RuleOf("ember", "anvil", Standard());
        Assert.Equal(("ember", "ember:anvil", ItemRarity.Legendary), (rule.Mod, rule.Id, rule.Max));
        Assert.Equal(["Weapon_*"], rule.Items!);
        Assert.Equal(3, rule.Steps[ItemRarity.Rare].Cost["ember-shard"]);
        Assert.True(rule.Steps[ItemRarity.Rare].Unlock);
        static ItemId? Lookup(string n) => n == "ember.ember-shard" ? new ItemId(42) : null;
        Assert.Equal(new ItemId(42), ItemUpgrades.Resolve("ember-shard", rule.Mod, Lookup));
    }

    [Fact]
    public void A_defined_rule_is_registered_by_its_id_and_goes_with_its_mod()
    {
        var mod = new ModBlocks("upgrade-def-test", 1) { ManifestClients = "none" };
        var handle = Compiler.Define(mod, "anvil", Standard());
        Assert.Equal("upgrade-def-test:anvil", handle.ToString());
        var rule = ItemUpgrades.Find("anvil", "upgrade-def-test");
        Assert.NotNull(rule);
        Assert.Same(rule, ItemUpgrades.Find(handle.ToString(), null));
        Assert.Equal(("upgrade-def-test", "upgrade-def-test:anvil"), (rule!.Mod, rule.Id));
        Assert.Null(ItemUpgrades.Find("anvil", "another-mod"));
        mod.Reset();
        Assert.Null(ItemUpgrades.Find("upgrade-def-test:anvil", null));
    }
}
