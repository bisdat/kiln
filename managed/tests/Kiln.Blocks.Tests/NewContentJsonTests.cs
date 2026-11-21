// SPDX-License-Identifier: MIT
// The JSON kiln.content's define() gets for a new item and a new recipe (NewContent.ToJson, which Register sends unchanged): pinned, so the
// boot generation's IR (chapter 08), which carries the same JSON, cannot drift from what a runtime definition registers.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Blocks.Tests;

public sealed class NewContentJsonTests
{
    [Fact]
    public void An_items_define_json_is_pinned()
    {
        var def = new ItemDefinition
        {
            Guid = Guid.Parse("0f8fad5b-d9cb-469f-a165-70867728950e"), Name = "boot-gallery.wood-token", BaseOn = "Material_T1_Wood_Logs",
            DisplayName = "Boot Token", Description = "a token", Lore = "old", Source = "boot", RolledLoot = true,
            Refs = new Dictionary<string, string> { ["model"] = "item:Material_T1_Wood_Logs" },
        };
        var expected = """
            {"guid":"0f8fad5b-d9cb-469f-a165-70867728950e","name":"boot-gallery.wood-token","base":"Material_T1_Wood_Logs","displayName":"Boot Token","description":"a token","lore":"old","source":"boot","rolledLoot":true,"refs":{"model":"item:Material_T1_Wood_Logs"}}
            """;
        Assert.Equal(expected.Trim(), NewContent.ToJson(def).ToJsonString());
    }

    [Fact]
    public void A_recipes_define_json_is_pinned()
    {
        var def = new RecipeDefinition
        {
            Guid = Guid.Parse("1f8fad5b-d9cb-469f-a165-70867728950e"), Name = "boot-gallery.token-recipe", Station = "workshop:alchemist",
            Inputs = [new RecipeIngredient("Material_T1_Wood_Logs", 2)], Outputs = [new RecipeIngredient("boot-gallery.wood-token", 1)],
            Duration = TimeSpan.FromSeconds(3),
        };
        var expected = """
            {"guid":"1f8fad5b-d9cb-469f-a165-70867728950e","name":"boot-gallery.token-recipe","station":"workshop:alchemist","inputs":[{"count":2,"item":"Material_T1_Wood_Logs"}],"outputs":[{"count":1,"item":"boot-gallery.wood-token"}],"duration":3,"unlock":"always"}
            """;
        Assert.Equal(expected.Trim(), NewContent.ToJson(def).ToJsonString());
    }

    private static RecipeDefinition Tea(int? water, IReadOnlyDictionary<string, object>? fields = null) => new()
    {
        Guid = Guid.Parse("1f0c8a52-3b7e-4d21-9a6c-2e5d8b4f7a10"), Name = "MyMod_Herb_Tea", Station = "0x6b8247d6",
        Inputs = [("Food_T7_raw_herb_Thyme", 2)], Outputs = [("Food_T7_dish_hydration_ThymeTea", 1)], Water = water, Fields = fields,
    };

    [Fact]
    public void A_recipes_Water_is_its_waterCharges()
    {
        // 0.2.0-dev.30.3 (thanks netjunk): RecipeDefinition.Water is RecipeInfo.waterCharges, charges per craft, sent as the
        // definition's "water", the key a data pack's recipe uses (kiln.content's recipe_water.h writes it after a
        // station-only recipe's requirements are cleared)
        var expected = """
            {"guid":"1f0c8a52-3b7e-4d21-9a6c-2e5d8b4f7a10","name":"MyMod_Herb_Tea","station":"0x6b8247d6","inputs":[{"count":2,"item":"Food_T7_raw_herb_Thyme"}],"outputs":[{"count":1,"item":"Food_T7_dish_hydration_ThymeTea"}],"water":5,"unlock":"always"}
            """;
        Assert.Equal(expected.Trim(), NewContent.ToJson(Tea(5)).ToJsonString());
        // none: no key at all (the template's water stays); 0 clears a base's; the largest is 1023
        Assert.Null(NewContent.ToJson(Tea(null))["water"]);
        Assert.Equal(0L, (long)NewContent.ToJson(Tea(0))["water"]!);
        Assert.Equal(1023L, (long)NewContent.ToJson(Tea(1023))["water"]!);
        // beside other fields, which stay fields
        var both = NewContent.ToJson(Tea(2, new Dictionary<string, object> { ["level"] = 3 }));
        Assert.Equal((3L, 2L), ((long)both["fields"]!["level"]!, (long)both["water"]!));
        Assert.Null(both["fields"]!["waterCharges"]);
    }

    [Fact]
    public void Water_outside_0_to_1023_or_given_twice_is_refused()
    {
        Assert.Contains("0..1023", Assert.Throws<ArgumentException>(() => NewContent.ToJson(Tea(-1))).Message);
        Assert.Contains("0..1023", Assert.Throws<ArgumentException>(() => NewContent.ToJson(Tea(1024))).Message);
        var twice = Assert.Throws<ArgumentException>(() => NewContent.ToJson(Tea(5, new Dictionary<string, object> { ["waterCharges"] = 5 })));
        Assert.Contains("not both", twice.Message);
        // the old way still works on its own
        var old = NewContent.ToJson(Tea(null, new Dictionary<string, object> { ["waterCharges"] = 4 }));
        Assert.Equal(4L, (long)old["fields"]!["waterCharges"]!);
        Assert.Null(old["water"]);
    }
}
