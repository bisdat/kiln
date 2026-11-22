// SPDX-License-Identifier: MIT
// A recipe in a tab of a game workshop's crafting menu (CraftingGroupRef, RecipeBlock.MenuGroup), and the game's tracking of a mod's items and
// recipes (KnowledgeBlock, RecipeBlock.TrackCrafted): what they lower to, what kiln.content and kiln-quests' boot build are sent, and the refusals by
// block and field. The menu and the knowledge table themselves are the game's: native tests (questpack) and the owner's check.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class MenuGroupTrackingTests : IDisposable
{
    // commits go nowhere: no Kiln core in a test process
    private readonly Lowered.ILoweredSink? _previous = Lowered._sinkOverride;
    public MenuGroupTrackingTests() => Lowered._sinkOverride = new NoSink();
    public void Dispose() => Lowered._sinkOverride = _previous;

    private sealed class NoSink : Lowered.ILoweredSink
    {
        public void Program(ulong owner, Guid guid, byte[] blob) { }
        public void Clone(ulong owner, string section, JsonObject def) { }
        public void Item(ulong owner, ItemDefinition d) { }
        public void Recipe(ulong owner, RecipeDefinition d) { }
    }

    private static ModBlocks Mod(string id, string clients = "kiln") => new(id, 1) { ManifestClients = clients };

    private const string Carpenter = "0xbddd74e6";

    private static RecipeBlock Recipe(CraftingGroupRef? group = null, string station = Carpenter, bool track = false, BlockIdentity? identity = null) => new()
    {
        Station = station, Output = "Material_T1_Wood_Logs", Needs = { ["Material_T1_Twigs"] = 1 }, MenuGroup = group, TrackCrafted = track, Identity = identity,
    };

    // ---- the crafting menu's tabs ----

    [Fact]
    public void The_carpenters_tabs_are_the_games()
    {
        // Kiln's extract of the game's menu configuration (client 26430625): the Carpenter's first tab "Essentials" holds the grappling hooks, its
        // "Tools & Components" tab the bucket
        Assert.Equal("0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e", CraftingGroups.Carpenter.Essentials);
        Assert.Equal("0xbddd74e6/35b13ff3-9156-4ec5-99bc-bee7d4508116", CraftingGroups.Carpenter.ToolsAndComponents);
        CraftingGroupRef g = CraftingGroups.Carpenter.Essentials;
        Assert.Equal(0xbddd74e6u, g.Workshop);
        Assert.Equal(new Guid("8c91ca52-be38-468c-ae74-bc6a5ffab28e"), g.Group);
        Assert.Equal(Guid.Empty, g.Set);
        Assert.Equal(0u, ((CraftingGroupRef)CraftingGroups.Hand.Essentials).Workshop);
    }

    [Fact]
    public void A_group_reads_and_writes_its_text_form()
    {
        var set = new Guid("51be23a7-616d-4158-96ed-815a47e1d507");
        var g = new CraftingGroupRef(0xbddd74e6, new Guid("8c91ca52-be38-468c-ae74-bc6a5ffab28e"), set);
        Assert.Equal("0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e/51be23a7-616d-4158-96ed-815a47e1d507", g.ToString());
        Assert.Equal(g, CraftingGroupRef.Parse(g.ToString()));
        foreach (var bad in new[] { "", "0xbddd74e6", "bddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e", "0xbddd74e6/00000000-0000-0000-0000-000000000000",
                                    "0x1bddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e", "0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e/x" })
            Assert.False(CraftingGroupRef.TryParse(bad, out _), bad);
        Assert.Throws<ArgumentException>(() => new CraftingGroupRef(1, Guid.Empty));
    }

    [Fact]
    public void A_recipe_in_a_tab_sends_kiln_content_the_group()
    {
        var r = Compiler.Lower(Mod("menu-a"), "logs", Recipe(CraftingGroups.Carpenter.Essentials)).Low.Json["recipes"]!.AsArray().Single()!;
        Assert.Equal(CraftingGroups.Carpenter.Essentials, r["menuGroup"]!.GetValue<string>());
        var def = new RecipeDefinition
        {
            Guid = Guid.Parse("1f8fad5b-d9cb-469f-a165-70867728950e"), Name = "menu.logs", Station = Carpenter, MenuGroup = CraftingGroups.Carpenter.Essentials,
            TrackCrafted = true, Inputs = [new RecipeIngredient("Material_T1_Twigs", 1)], Outputs = [new RecipeIngredient("Material_T1_Wood_Logs", 1)],
        };
        var expected = """
            {"guid":"1f8fad5b-d9cb-469f-a165-70867728950e","name":"menu.logs","station":"0xbddd74e6","trackCrafted":true,"menuGroup":"0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e","inputs":[{"count":1,"item":"Material_T1_Twigs"}],"outputs":[{"count":1,"item":"Material_T1_Wood_Logs"}],"unlock":"always"}
            """;
        Assert.Equal(expected.Trim(), NewContent.ToJson(def).ToJsonString());
    }

    [Fact]
    public void A_tab_of_another_workshops_menu_is_refused()
    {
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(Mod("menu-b"), "logs", Recipe(CraftingGroups.Blacksmith.ToolsAndComponents)));
        Assert.Equal("menuGroup", e.Field);
        Assert.Contains("0x41bf48ce", e.Problem, StringComparison.Ordinal);
        // the player's own menu is workshop 0
        Compiler.Lower(Mod("menu-c"), "logs", Recipe(CraftingGroups.Hand.Essentials, Stations.Hand));
        Assert.Throws<ArgumentException>(() => NewContent.ToJson(new RecipeDefinition
        {
            Guid = Guid.NewGuid(), Name = "x", Station = "hand", MenuGroup = CraftingGroups.Carpenter.Essentials, Inputs = [("Material_T1_Twigs", 1)], Outputs = [("Material_T1_Wood_Logs", 1)],
        }));
    }

    [Fact]
    public void The_JSON_form_names_a_tab_by_its_text()
    {
        var r = BlockJson.Parse<RecipeBlock>("""{ "station": "0xbddd74e6", "output": "Material_T1_Wood_Logs", "needs": { "Material_T1_Twigs": 1 }, "menuGroup": "0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e", "trackCrafted": true }""");
        Assert.Equal((CraftingGroupRef)CraftingGroups.Carpenter.Essentials, r.MenuGroup);
        Assert.True(r.TrackCrafted);
        Assert.Throws<System.Text.Json.JsonException>(() => BlockJson.Parse<RecipeBlock>("""{ "output": "x", "menuGroup": "Tools & Components" }"""));
    }

    // ---- tracking ----

    [Fact]
    public void An_items_pickup_is_tracked_under_its_id()
    {
        var mod = Mod("track-a", "mod");
        var tool = Guid.NewGuid();  // legacy ids are held for the process once defined: a GUID of this test's own
        Compiler.Define(mod, "staff", new ItemBlock { Name = "Staff", Tool = new ToolBlock { Harvests = Harvest.Stone }, Identity = BlockIdentity.Legacy(tool) });
        var k = Compiler.Lower(mod, "staff-found", new KnowledgeBlock { Trigger = KnowledgeTrigger.Pickup, Of = "staff" }).Low.Json["knowledge"]!;
        Assert.Equal($"0x{NewContent.IdOf(tool):x8}", k["id"]!.GetValue<string>());
        Assert.Equal("pickup", k["track"]!.GetValue<string>());
        Assert.Equal("both", k["scope"]!.GetValue<string>());
        Assert.Equal("track.staff-found", k["entry"]!.GetValue<string>());
    }

    [Fact]
    public void A_recipes_craft_is_tracked_per_player_and_needs_the_mark()
    {
        var mod = Mod("track-b", "mod");
        var guid = Guid.NewGuid();
        Compiler.Define(mod, "make", Recipe(track: true, identity: BlockIdentity.Legacy(guid)));
        var k = Compiler.Lower(mod, "made", new KnowledgeBlock { Trigger = KnowledgeTrigger.Craft, Of = "make" }).Low.Json["knowledge"]!;
        Assert.Equal($"0x{NewContent.IdOf(guid):x8}", k["id"]!.GetValue<string>());
        Assert.Equal("player", k["scope"]!.GetValue<string>());

        Compiler.Define(mod, "unmarked", Recipe());
        Assert.Equal("of", Assert.Throws<BlockError>(() => Compiler.Lower(mod, "x", new KnowledgeBlock { Trigger = KnowledgeTrigger.Craft, Of = "unmarked" })).Field);
        Assert.Equal("scope", Assert.Throws<BlockError>(() => Compiler.Lower(mod, "y", new KnowledgeBlock { Trigger = KnowledgeTrigger.Craft, Of = "make", Scope = TrackingScope.World })).Field);
        Assert.Equal("trigger", Assert.Throws<BlockError>(() => Compiler.Lower(mod, "z", new KnowledgeBlock { Trigger = KnowledgeTrigger.Pickup, Of = "make" })).Field);
    }

    [Fact]
    public void An_items_own_recipe_is_tracked_by_the_items_name()
    {
        var mod = Mod("track-c", "mod");
        Compiler.Define(mod, "staff", new ItemBlock { Name = "Staff", Tool = new ToolBlock { Harvests = Harvest.Stone }, Recipe = Recipe(track: true) with { Output = null } });
        var k = Compiler.Lower(mod, "staff-made", new KnowledgeBlock { Trigger = KnowledgeTrigger.Craft, Of = "staff" }).Low.Json["knowledge"]!;
        Assert.Equal($"0x{BlockIds.Fnv(BlockIds.Derive("track-c", "recipe", "staff")):x8}", k["id"]!.GetValue<string>());
    }

    [Fact]
    public void An_identity_alone_is_tracked_and_one_of_of_or_identity_is_needed()
    {
        var mod = Mod("track-d", "mod");
        var k = Compiler.Lower(mod, "brush", new KnowledgeBlock { Trigger = KnowledgeTrigger.Pickup, Identity = BlockIdentity.Legacy(3155237504u), Scope = TrackingScope.Player }).Low.Json["knowledge"]!;
        Assert.Equal("0xbc111a80", k["id"]!.GetValue<string>());
        Assert.Equal("player", k["scope"]!.GetValue<string>());
        Assert.Equal("of", Assert.Throws<BlockError>(() => Compiler.Lower(mod, "none", new KnowledgeBlock { Trigger = KnowledgeTrigger.Pickup })).Field);
        Assert.Equal("of", Assert.Throws<BlockError>(() => Compiler.Lower(mod, "both", new KnowledgeBlock { Trigger = KnowledgeTrigger.Pickup, Of = "x", Identity = BlockIdentity.Legacy(1u) })).Field);
    }

    [Fact]
    public void Tracking_asks_players_for_the_mod()
    {
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(Mod("track-e", "kiln"), "id", new KnowledgeBlock { Trigger = KnowledgeTrigger.Pickup, Identity = BlockIdentity.Legacy(7u) }));
        Assert.Contains("clients", e.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void The_JSON_form_is_the_same_block()
    {
        var k = BlockJson.Parse<KnowledgeBlock>("""{ "trigger": "craft", "of": "make", "scope": "player" }""");
        Assert.Equal(new KnowledgeBlock { Trigger = KnowledgeTrigger.Craft, Of = "make", Scope = TrackingScope.Player }, k);
    }
}
