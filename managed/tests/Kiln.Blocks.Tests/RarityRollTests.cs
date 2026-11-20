// SPDX-License-Identifier: MIT
// Rarity at creation (package P5 of the equipment wave): RarityRollBlock checked by field, its JSON form, its lowering,
// the resolution of its items against the item registry (names, "other-mod:name", patterns, later definitions win) and the
// native entries kiln.items v8 rarity_rolls takes (kiln_items.h KilnItemsRarityRoll). The roll itself is kiln.items'
// createPide hook: native/services/items/tests/pide_test.cpp.
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed unsafe class RarityRollTests
{
    private static readonly Dictionary<Rarity, double> Spread = new()
    {
        [Rarity.Common] = 50, [Rarity.Uncommon] = 30, [Rarity.Rare] = 15, [Rarity.Epic] = 4, [Rarity.Legendary] = 1,
    };

    private static JsonObject Lower(RarityRollBlock b) =>
        Compiler.Lower(new ModBlocks("rolls-test", 1) { ManifestClients = "mod" }, "ember-rolls", b).Low.Json;

    private static string Field(RarityRollBlock b) => Assert.Throws<BlockError>(() => Lower(b)).Field ?? "";

    [Fact]
    public void A_rule_lowers_to_its_id_items_sources_and_five_weights()
    {
        var j = Lower(new RarityRollBlock { Items = ["Weapon_*", "ember-sword"], Weights = Spread });
        var r = j["rarityRoll"]!;
        Assert.Equal("rolls-test:ember-rolls", (string)r["id"]!);
        Assert.Equal(["Weapon_*", "ember-sword"], r["items"]!.AsArray().Select(n => (string)n!));
        Assert.Equal("Craft, Loot", (string)r["sources"]!);
        Assert.Equal(50, (double)r["weights"]!["Common"]!);
        Assert.Equal(1, (double)r["weights"]!["Legendary"]!);
    }

    [Fact]
    public void A_wrong_rule_is_refused_by_field()
    {
        Assert.Equal("items", Field(new RarityRollBlock { Items = [], Weights = Spread }));
        Assert.Equal("items[0]", Field(new RarityRollBlock { Items = [" "], Weights = Spread }));
        Assert.Equal("sources", Field(new RarityRollBlock { Items = ["a"], Sources = ItemSource.None, Weights = Spread }));
        Assert.Equal("sources", Field(new RarityRollBlock { Items = ["a"], Sources = (ItemSource)8, Weights = Spread }));
        Assert.Equal("weights", Field(new RarityRollBlock { Items = ["a"], Weights = new Dictionary<Rarity, double>() }));
        Assert.Equal("weights", Field(new RarityRollBlock { Items = ["a"], Weights = new Dictionary<Rarity, double> { [Rarity.Rare] = 0 } }));
        Assert.Equal("weights", Field(new RarityRollBlock { Items = ["a"], Weights = new Dictionary<Rarity, double> { [(Rarity)5] = 1 } }));
        Assert.Equal("weights.epic", Field(new RarityRollBlock { Items = ["a"], Weights = new Dictionary<Rarity, double> { [Rarity.Epic] = -1, [Rarity.Rare] = 1 } }));
        Assert.Equal("weights.legendary", Field(new RarityRollBlock { Items = ["a"], Weights = new Dictionary<Rarity, double> { [Rarity.Legendary] = double.NaN } }));
    }

    [Fact]
    public void The_json_form_is_the_record()
    {
        var b = BlockJson.Parse<RarityRollBlock>("""{ "items": ["my-mod:*"], "sources": "craft, give", "weights": { "common": 3, "legendary": 1 } }""");
        Assert.Equal(["my-mod:*"], b.Items.Select(i => i.Value));
        Assert.Equal(ItemSource.Craft | ItemSource.Give, b.Sources);
        Assert.Equal(3, b.Weights[Rarity.Common]);
        Assert.Equal(1, b.Weights[Rarity.Legendary]);
        Assert.Equal(ItemSource.Craft | ItemSource.Loot, BlockJson.Parse<RarityRollBlock>("""{ "items": ["a"], "weights": { "rare": 1 } }""").Sources);
        Assert.ThrowsAny<System.Text.Json.JsonException>(() => BlockJson.Parse<RarityRollBlock>("""{ "items": ["a"], "weights": { "rare": 1 }, "chance": 1 }"""));
    }

    private static readonly (uint Id, string Name)[] Catalog =
    [
        (1, "Weapon_T1_1H_Sword_Craft"), (2, "Weapon_T2_2H_Axe_Craft"), (3, "Armor_T2_Warrior_Tank_Chest"),
        (10, "ember.sword"), (11, "ember.ring"), (20, "other-mod.blade"),
    ];

    private static uint? IdOf(string name) => Catalog.Where(c => c.Name == name).Select(c => (uint?)c.Id).FirstOrDefault();

    private static RarityRollRuntime.Def Def(string mod, string name, long seq, RarityRollBlock b) =>
        new($"{mod}:{name}", mod, name, seq, new BlockHandle<RarityRollBlock>(mod, name, Guid.Empty, $"{mod}:{name}", b), b);

    [Fact]
    public void Items_resolve_by_game_name_mod_name_other_mod_and_pattern()
    {
        Assert.Equal([1u, 2u], RarityRollRuntime.Match("ember", "Weapon_*", Catalog, IdOf));
        Assert.Equal([10u, 11u], RarityRollRuntime.Match("ember", "ember:*", Catalog, IdOf));
        Assert.Equal([10u], RarityRollRuntime.Match("ember", "sword", Catalog, IdOf));           // this mod's block name
        Assert.Equal([3u], RarityRollRuntime.Match("ember", "Armor_T2_Warrior_Tank_Chest", Catalog, IdOf));
        Assert.Equal([20u], RarityRollRuntime.Match("ember", "other-mod:blade", Catalog, IdOf));
        Assert.Empty(RarityRollRuntime.Match("ember", "other-mod:sword", Catalog, IdOf));       // qualified: no fallback to this mod
        Assert.Empty(RarityRollRuntime.Match("ember", "Nothing_*", Catalog, IdOf));
    }

    [Fact]
    public void A_later_definition_wins_an_item_both_name_and_unknown_names_are_noted_once_per_entry()
    {
        var notes = new List<string>();
        var first = Def("ember", "all", 1, new RarityRollBlock { Items = ["Weapon_*", "ghost"], Weights = new Dictionary<Rarity, double> { [Rarity.Common] = 1 } });
        var later = Def("other-mod", "swords", 2, new RarityRollBlock
        {
            Items = ["Weapon_T1_1H_Sword_Craft"], Sources = ItemSource.Loot, Weights = new Dictionary<Rarity, double> { [Rarity.Epic] = 2, [Rarity.Legendary] = 1 },
        });
        var rules = RarityRollRuntime.Resolve([first, later], Catalog, IdOf, (_, text) => notes.Add(text));
        Assert.Equal([1u, 2u], rules.Select(r => r.Item));
        Assert.Equal(new ItemRarityRolls.Rule(1, ItemsNative.RollLoot, 0, 0, 0, 2, 1), rules[0]);
        Assert.Equal(new ItemRarityRolls.Rule(2, ItemsNative.RollCraft | ItemsNative.RollLoot, 1, 0, 0, 0, 0), rules[1]);
        Assert.Contains(notes, n => n.Contains("\"ghost\" names no item"));
        Assert.Contains(notes, n => n.Contains("named by ember:all and other-mod:swords"));
    }

    [Fact]
    public void Sources_map_to_the_native_bits()
    {
        Assert.Equal(ItemsNative.RollCraft | ItemsNative.RollGive, RarityRollRuntime.SourcesOf(ItemSource.Craft | ItemSource.Give));
        Assert.Equal(ItemsNative.RollLoot, RarityRollRuntime.SourcesOf(ItemSource.Loot));
    }

    [Fact]
    public void The_native_entries_and_table_match_kiln_items_h()
    {
        // kiln_items.h: KilnItemsRarityRoll 32 bytes (weights at 8), KilnItemsRollStatus 248 (rolled at 24); the v8 table's
        // rarity_rolls at 320 and roll_status at 328, after v7's 296 bytes and gear_write/gear_read/gear_status.
        Assert.Equal(32, sizeof(ItemsNative.RarityRoll));
        Assert.Equal(8, (int)Marshal.OffsetOf<ItemsNative.RarityRoll>("Weights"));
        Assert.Equal(248, sizeof(ItemsNative.RollStatus));
        Assert.Equal(24, (int)Marshal.OffsetOf<ItemsNative.RollStatus>("Rolled"));
        Assert.Equal(296, sizeof(ItemsNative.TableV7));
        Assert.Equal(320, (int)Marshal.OffsetOf<ItemsNative.TableV8>("RarityRolls"));
        Assert.Equal(368, sizeof(ItemsNative.TableV8));  // P3's gear_upgrade follows roll_status (336), P7's scales follow it (344)
        var e = ItemRarityRolls.Entries([new ItemRarityRolls.Rule(7, ItemsNative.RollCraft, 50, 30, 15, 4, 1)]);
        Assert.Equal(7u, e[0].Item);
        Assert.Equal(ItemsNative.RollCraft, e[0].Sources);
        Assert.Equal(15f, e[0].Weights[2]);
        Assert.Equal(1f, e[0].Weights[4]);
        Assert.Equal(0u, e[0].Reserved);
    }
}
