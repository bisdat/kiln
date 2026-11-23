// SPDX-License-Identifier: MIT
// Legacy identities: an item or recipe block that keeps the GUID (and so the game id) another loader gave it, so that what
// players made under that loader is still the same item. Golden ids are the game's FNV-1a of the GUID, as the other loader
// computed them; refusals come by block and field.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class LegacyIdentityTests : IDisposable
{
    // commits go nowhere: no Kiln core in a test process
    private readonly Lowered.ILoweredSink? _previous = Lowered._sinkOverride;
    public LegacyIdentityTests() => Lowered._sinkOverride = new NoSink();
    public void Dispose() => Lowered._sinkOverride = _previous;

    private sealed class NoSink : Lowered.ILoweredSink
    {
        public void Program(ulong owner, Guid guid, byte[] blob) { }
        public void Clone(ulong owner, string section, JsonObject def) { }
        public void Item(ulong owner, ItemDefinition d) { }
        public void Recipe(ulong owner, RecipeDefinition d) { }
    }

    private static readonly Guid Tool = new("c52ceb9f-b1b0-4b4b-a801-52bf8340c001");
    private static readonly Guid ToolRecipe = new("c52ceb9f-b1b0-4b4b-a801-52bf8340c00a");

    private static ModBlocks Mod(string id) => new(id, 1) { ManifestClients = "kiln" };

    private static ItemBlock Staff(BlockIdentity? identity = null, RecipeBlock? recipe = null) => new()
    {
        Name = "Staff", Tool = new ToolBlock { Harvests = Harvest.Stone }, Identity = identity, Recipe = recipe,
    };

    private static RecipeBlock MakeStaff(BlockIdentity? identity = null, ItemRef? output = null) => new()
    {
        Station = "0xbddd74e6", Output = output, Identity = identity, Needs = { ["Material_T1_Wood_Logs"] = 1 },
    };

    private static JsonNode Single(JsonObject low, string section) => low[section]!.AsArray().Single()!;

    // ---- the ids ----

    [Theory]
    [InlineData("c52ceb9f-b1b0-4b4b-a801-52bf8340c001", 3205570361u)]  // the tool
    [InlineData("c52ceb9f-b1b0-4b4b-a801-52bf8340c002", 3155237504u)]  // its brushes
    [InlineData("c52ceb9f-b1b0-4b4b-a801-52bf8340c003", 3172015123u)]
    [InlineData("c52ceb9f-b1b0-4b4b-a801-52bf8340c004", 3255903218u)]
    [InlineData("c52ceb9f-b1b0-4b4b-a801-52bf8340c00a", 3289458456u)]  // its recipe
    public void A_legacy_GUID_has_the_game_id_the_other_loader_gave_it(string guid, uint id)
    {
        var identity = BlockIdentity.Legacy(new Guid(guid));
        Assert.Equal(new Guid(guid), identity.Guid);
        Assert.Equal(id, identity.Id);
        Assert.Equal(id, NewContent.IdOf(identity.Guid));
    }

    [Fact]
    public void A_legacy_id_alone_gets_a_GUID_with_that_id()
    {
        var identity = BlockIdentity.Legacy(3205570361u);
        Assert.Equal(3205570361u, identity.Id);
        Assert.Equal(3205570361u, NewContent.IdOf(identity.Guid));
        Assert.Equal(identity, BlockIdentity.Legacy(3205570361u));  // the same id, the same GUID: on every machine
    }

    [Fact]
    public void An_empty_GUID_or_id_0_is_no_identity()
    {
        Assert.Throws<ArgumentException>(() => BlockIdentity.Legacy(Guid.Empty));
        Assert.Throws<ArgumentException>(() => BlockIdentity.Legacy(0u));
    }

    // ---- what a block becomes ----

    [Fact]
    public void An_item_with_a_legacy_identity_is_defined_under_that_GUID()
    {
        var (handle, low, _) = Compiler.Lower(Mod("legacy-item"), "staff", Staff(BlockIdentity.Legacy(Tool)));
        Assert.Equal(Tool.ToString(), Single(low.Json, "items")["guid"]!.GetValue<string>());
        Assert.Equal(Tool, handle.Guid);
        Assert.Equal("legacy-item.staff", handle.GameName);  // the name stays the block's
    }

    [Fact]
    public void An_item_without_one_keeps_its_derived_GUID()
    {
        var (handle, _, _) = Compiler.Lower(Mod("legacy-plain"), "staff", Staff());
        Assert.Equal(BlockIds.Derive("legacy-plain", "item", "staff"), handle.Guid);
    }

    [Fact]
    public void An_items_own_recipe_and_a_recipe_of_its_own_take_their_legacy_GUID()
    {
        var own = Compiler.Lower(Mod("legacy-own"), "staff", Staff(BlockIdentity.Legacy(Tool), MakeStaff(BlockIdentity.Legacy(ToolRecipe)))).Low.Json;
        Assert.Equal(ToolRecipe.ToString(), Single(own, "recipes")["guid"]!.GetValue<string>());

        var (handle, low, _) = Compiler.Lower(Mod("legacy-alone"), "make-staff", MakeStaff(BlockIdentity.Legacy(ToolRecipe), "Material_T1_Wood_Logs"));
        Assert.Equal(ToolRecipe.ToString(), Single(low.Json, "recipes")["guid"]!.GetValue<string>());
        Assert.Equal(ToolRecipe, handle.Guid);
    }

    // ---- refusals ----

    [Fact]
    public void Two_blocks_of_one_mod_with_the_same_identity_are_refused()
    {
        // ids are held for the whole process once a block is in (two mods never share one): each test takes its own GUID
        var guid = Guid.NewGuid();
        var mod = Mod("legacy-twice");
        Compiler.Define(mod, "staff", Staff(BlockIdentity.Legacy(guid)));
        Compiler.Define(mod, "staff", Staff(BlockIdentity.Legacy(guid)));  // the same block again (a reload) is fine
        var e = Assert.Throws<BlockError>(() => Compiler.Define(mod, "other-staff", Staff(BlockIdentity.Legacy(guid))));
        Assert.Equal("identity", e.Field);
        Assert.Contains("item staff", e.Problem, StringComparison.Ordinal);
        Assert.Contains($"0x{NewContent.IdOf(guid):x8}", e.Problem, StringComparison.Ordinal);
    }

    [Fact]
    public void An_identity_another_mod_holds_is_refused()
    {
        var guid = Guid.NewGuid();
        Compiler.Lower(Mod("legacy-tried"), "staff", Staff(BlockIdentity.Legacy(guid)));  // lowered, never committed: holds nothing
        Compiler.Define(Mod("legacy-first"), "staff", Staff(BlockIdentity.Legacy(guid)));
        var e = Assert.Throws<BlockError>(() => Compiler.Define(Mod("legacy-second"), "staff", Staff(BlockIdentity.Legacy(guid))));
        Assert.Equal("identity", e.Field);
        Assert.Contains("legacy-first", e.Problem, StringComparison.Ordinal);
    }

    [Fact]
    public void Items_and_recipes_have_ids_of_their_own()
    {
        // the game keeps items and recipes in different registries: one GUID may be an item's and a recipe's
        var guid = Guid.NewGuid();
        var mod = Mod("legacy-spaces");
        Compiler.Define(mod, "staff", Staff(BlockIdentity.Legacy(guid)));
        Compiler.Define(mod, "make-staff", MakeStaff(BlockIdentity.Legacy(guid), "staff"));
    }

    [Fact]
    public void A_recipe_identity_twice_in_one_mod_is_refused_by_its_field()
    {
        var guid = Guid.NewGuid();
        var mod = Mod("legacy-recipes");
        Compiler.Define(mod, "a", MakeStaff(BlockIdentity.Legacy(guid), "Material_T1_Wood_Logs"));
        var e = Assert.Throws<BlockError>(() => Compiler.Define(mod, "b", Staff(null, MakeStaff(BlockIdentity.Legacy(guid)))));
        Assert.Equal("recipe.identity", e.Field);
    }

    // ---- the data file says what the C# says ----

    [Fact]
    public void The_JSON_forms_are_the_same_identities()
    {
        var byGuid = BlockJson.Parse<ItemBlock>("""{ "name": "Staff", "tool": { "harvests": "stone" }, "identity": { "legacy": "c52ceb9f-b1b0-4b4b-a801-52bf8340c001" } }""");
        Assert.Equal(BlockIdentity.Legacy(Tool), byGuid.Identity);
        var byId = BlockJson.Parse<RecipeBlock>("""{ "output": "Material_T1_Wood_Logs", "needs": { "Material_T1_Twigs": 1 }, "identity": { "legacyId": 3289458456 } }""");
        Assert.Equal(BlockIdentity.Legacy(3289458456u), byId.Identity);
        Assert.Throws<System.Text.Json.JsonException>(() => BlockJson.Parse<RecipeBlock>("""{ "output": "x", "identity": { "legacy": "c52ceb9f-b1b0-4b4b-a801-52bf8340c001", "legacyId": 1 } }"""));
        Assert.Throws<System.Text.Json.JsonException>(() => BlockJson.Parse<RecipeBlock>("""{ "output": "x", "identity": { "legacy": "00000000-0000-0000-0000-000000000000" } }"""));
    }
}
