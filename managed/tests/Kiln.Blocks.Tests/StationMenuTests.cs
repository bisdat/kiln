// SPDX-License-Identifier: MIT
// Station menus with tabs of their own (chapter 16a, dev31 out/16a-station-tabs/DESIGN.md), with no game: what a station's
// blocks lower to without Tabs or MenuKey (pinned by goldens in Goldens/stations/, written from the compiler before 16a),
// the MenuKey binding table, the normalized record, its refusals, and the layers through the store facade on a fake backend.
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using AffixBlockFace;
using Xunit;

namespace Kiln.BlockTests;

public sealed class StationMenuTests : IDisposable
{
    // ---- blocks without Tabs or MenuKey lower as before ------------------------------------------------------------

    private const string GoldenMod = "golden-stations";

    private static string GoldenDir([CallerFilePath] string self = "") => Path.Combine(Path.GetDirectoryName(self)!, "Goldens", "stations");

    /// <summary>The station cases, by golden name: (block type, the block as blocks/*.json writes it). An item case lowers after the
    /// "forge" station case, so that its recipe can name the station.</summary>
    internal static readonly (string Name, Type Type, string Json)[] GoldenCases =
    [
        ("forge", typeof(StationBlock), """
            { "name": "Rune Forge", "kind": "forge", "processing": { "slots": 6 }, "menu": { "like": "workbench" },
              "recipes": [ { "output": "Material_T1_Metal_Scraps", "needs": { "Material_T1_Wood_Logs": 1 }, "seconds": 6 },
                           { "output": "Material_T1_Metal_Scraps", "needs": { "Material_T1_Wood_Logs": 2 }, "seconds": 8 } ] }
            """),
        ("bench-shared", typeof(StationBlock), """
            { "name": "Bench", "kind": "workbench", "recipes": [ { "output": "Material_T1_Wood_Logs", "needs": { "Material_T1_Twigs": 2 } } ] }
            """),
        ("bench-like", typeof(StationBlock), """{ "name": "Like Bench", "kind": "workbench", "menu": { "like": "carpenter" } }"""),
        ("bolts", typeof(ItemBlock), """
            { "name": "Bolts", "recipe": { "station": "forge", "needs": { "Material_T1_Metal_Scraps": 2 }, "seconds": 4 } }
            """),
    ];

    public static TheoryData<string> GoldenNames => new(GoldenCases.Select(c => c.Name));

    private static readonly System.Reflection.MethodInfo ParseOpen = typeof(BlockJson).GetMethod("Parse", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
    private static readonly System.Reflection.MethodInfo LowerOpen = typeof(StationMenuTests).GetMethod(nameof(LowerTyped), System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;

    private static (BlockHandle Handle, JsonObject Json) LowerTyped<T>(ModBlocks mod, string name, T block) where T : class, IBlock
    {
        var (handle, low, _) = Compiler.Lower(mod, name, block);
        return (handle, low.Json);
    }

    private static JsonObject GoldenLowered(string name)
    {
        var mod = new ModBlocks(GoldenMod, 1) { ManifestClients = "mod" };
        JsonObject? made = null;
        foreach (var c in GoldenCases)
        {
            var block = ParseOpen.MakeGenericMethod(c.Type).Invoke(null, [c.Json])!;
            (BlockHandle Handle, JsonObject Json) r;
            try { r = ((BlockHandle, JsonObject))LowerOpen.MakeGenericMethod(c.Type).Invoke(null, [mod, c.Name, block])!; }
            catch (System.Reflection.TargetInvocationException e) when (e.InnerException != null) { throw e.InnerException; }
            mod.Handles[c.Name] = r.Handle;
            if (c.Name == name) made = r.Json;
        }
        return made!;
    }

    [Theory]
    [MemberData(nameof(GoldenNames))]
    public void A_station_without_tabs_or_menu_keys_lowers_to_its_golden(string name)
    {
        var made = GoldenLowered(name);
        var file = Path.Combine(GoldenDir(), name + ".json");
        // a missing golden says what it would hold: written once from the compiler before station menus, never refreshed to pass
        if (!File.Exists(file)) Assert.Fail($"no golden {file}; the compiler made:\n----8<----\n{ItemCompileGoldenTests.Text(made)}----8<----");
        ItemCompileGoldenTests.Compare(GoldenDir(), name, made);
    }

    // ---- the test's world: a ready pass that runs when the test says, a store that keeps layers as kiln.stationmenus does -----

    private readonly MenuReadyHost _host = new();
    private readonly IReadyHost _wasHost = BlockRuntime.Host;
    private readonly FakeMenuStore _store = new();
    private readonly NullSink _sink = new();
    private readonly ModContext? _outer = ModContext.CurrentMod;
    private readonly List<ModContext> _contexts = [];

    public StationMenuTests()
    {
        BlockRuntime.Host = _host;
        StationMenuCompiler.ResetForTests();
        StationMenuRuntime.ResetForTests();
        StationMenuStore.ResetForTests();
        StationMenuStore.TestBackend = _store;
        StationMenuRuntime.AutoTick = false;
        Lowered._sinkOverride = _sink;
    }

    public void Dispose()
    {
        Lowered._sinkOverride = null;
        ModContext.CurrentMod = _outer;
        StationMenuStore.TestBackend = null;
        StationMenuStore.ResetForTests();
        StationMenuRuntime.ResetForTests();
        StationMenuCompiler.ResetForTests();
        StationMenuRuntime.AutoTick = true;
        BlockRuntime.Host = _wasHost;
    }

    private static ModBlocks Ledger(string id, ulong owner = 7) => new(id, owner) { ManifestClients = "mod", OwnedByMod = true };

    private ModContext Context(string id, ulong owner)
    {
        var c = HostAccess.CreateContext(new ModInfo(id, id, "1", AppContext.BaseDirectory), owner, AppContext.BaseDirectory);
        _contexts.Add(c);
        return c;
    }

    private static UiText Text(params (string Language, string Text)[] texts) => new(texts.ToDictionary(t => t.Language, t => t.Text));

    private static StationMenuTabBlock Tab(string key, params StationMenuSetBlock[] sets) => new() { Key = key, Name = Text(("en", "Metal"), ("de", "Metall")), Sets = sets };

    private static StationMenuSetBlock Set(string key, params string[] recipes) => new() { Key = key, Recipes = recipes.Select(r => (StationRecipeRef)r).ToList() };

    /// <summary>The chapter's example: station forge (Like workbench) with the station-list recipe reclaim, and the item bolts made at it.</summary>
    private static (ModBlocks Mod, BlockHandle<StationBlock> Forge) Forge(IReadOnlyList<StationMenuTabBlock>? tabs, string mod = "menu-example", ulong owner = 7, ModBlocks? into = null)
    {
        var m = into ?? Ledger(mod, owner);
        var forge = Compiler.Define(m, "forge", new StationBlock
        {
            Name = "Rune Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock(),
            Menu = new MenuBlock { Like = StationKind.Workbench, Tabs = tabs },
            Recipes = [new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = "reclaim", Needs = { ["Material_T1_Wood_Logs"] = 1 }, Seconds = 6 }],
        });
        Compiler.Define(m, "bolts", new ItemBlock { Name = "Bolts", Recipe = new RecipeBlock { Station = "forge", MenuKey = "bolts", Needs = { ["Material_T1_Metal_Scraps"] = 2 }, Seconds = 4 } });
        return (m, forge);
    }

    private static readonly StationMenuTabBlock[] ExampleTabs = [Tab("metal", Set("main", "reclaim", "bolts"))];

    /// <summary>Every recipe with a MenuKey is in the game, in this state.</summary>
    private void InGame(string state = "registered")
    {
        _host.Items.Clear();
        foreach (var b in StationMenuCompiler.LiveBindings())
            _host.Items.Add(new ContentDefinition(ContentKind.Recipes, b.Recipe, b.Id, b.Name, state, ["server"], "test", null));
    }

    private void SetState(string recipeName, string state)
    {
        var i = _host.Items.FindIndex(d => d.Name == recipeName);
        _host.Items[i] = _host.Items[i] with { State = state };
    }

    private static uint RecipeId(string mod, string key) => BlockIds.Fnv(BlockIds.Derive(mod, "recipe", key));

    private static string Field(Action a) => Assert.Throws<BlockError>(a).Field ?? "";

    private static BlockHandle<StationBlock> Station(ModBlocks m, string name, StationBlock s) => Compiler.Define(m, name, s);

    // the record of the chapter's example (DESIGN.md section 2): the probe fixture station-wire-example.json with its keys sorted and
    // iconSourceGroup replaced by iconFromTab (DESIGN.md 7.2). The fixture's id for bolts (3696751007) is the FNV-1a of
    // Derive("menu-example", "recipe", "bolts.recipe"); the compiler lowers an item's recipe under the item's key ("bolts"), unchanged
    // here, so the record carries 391088916.
    private const string ExampleRecord =
        "{\"baseWorkshopId\":1302892403,\"definition\":{\"id\":\"724d7b7b-a362-566b-b0a7-5088f219266f\",\"key\":\"forge.menu\",\"mod\":\"menu-example\"},\"format\":1," +
        "\"origin\":{\"mod\":\"menu-example\",\"stationKey\":\"forge\",\"workshopGuid\":\"66b9e41a-34d5-51c4-8cec-36c189ab26cc\",\"workshopId\":2947643217}," +
        "\"tabs\":[{\"iconFromTab\":0,\"id\":\"27d00d31-a7ce-56d9-ab60-e2c50c02427d\",\"key\":\"metal\",\"name\":{\"de\":\"Metall\",\"en\":\"Metal\"}," +
        "\"sets\":[{\"heading\":null,\"id\":\"90598091-ecc6-50ab-bcb9-0941439c2e80\",\"key\":\"main\"," +
        "\"recipes\":[{\"key\":\"menu-example:reclaim\",\"recipeId\":3112649734},{\"key\":\"menu-example:bolts\",\"recipeId\":391088916}]}]}]}";

    // ---- the chapter's example and the ids ---------------------------------------------------------------------------

    [Fact]
    public void The_forge_example_lowers_to_the_fixture_record_and_is_stored_at_content_ready()
    {
        Forge(ExampleTabs);
        Assert.Empty(_store.Layers);  // nothing before content is ready
        InGame();
        _host.Apply();
        var layer = Assert.Single(_store.Layers.Values);
        Assert.Equal(ExampleRecord, layer.Record);
        Assert.Equal(7ul, layer.Owner);
        Assert.Equal(2947643217u, layer.Workshop);
        Assert.Equal(1ul, StationMenuStore.Generation);
        Assert.Equal("applied", StationMenuRuntime.Find(7, new Guid("724d7b7b-a362-566b-b0a7-5088f219266f"))!.State);
    }

    [Fact]
    public void Ids_derive_from_the_mod_and_the_keys()
    {
        Assert.Equal(new Guid("66b9e41a-34d5-51c4-8cec-36c189ab26cc"), BlockIds.Derive("menu-example", "workshop", "forge"));
        Assert.Equal(2947643217u, BlockIds.Fnv(new Guid("66b9e41a-34d5-51c4-8cec-36c189ab26cc")));
        Assert.Equal(new Guid("724d7b7b-a362-566b-b0a7-5088f219266f"), BlockIds.Derive("menu-example", "menu-layout", "forge.menu"));
        Assert.Equal(new Guid("27d00d31-a7ce-56d9-ab60-e2c50c02427d"), BlockIds.Derive("menu-example", "menu-tab", "forge.menu.metal"));
        Assert.Equal(new Guid("90598091-ecc6-50ab-bcb9-0941439c2e80"), BlockIds.Derive("menu-example", "menu-set", "forge.menu.metal.main"));
        Assert.Equal(3112649734u, RecipeId("menu-example", "forge.makes.Material_T1_Metal_Scraps"));
        Assert.Equal(391088916u, RecipeId("menu-example", "bolts"));
        // the menu's own ids, through the compiler
        var menu = StationMenuCompiler.Check("menu-example", "forge.menu", "menu-example", ExampleTabs, "menu.");
        Assert.Equal(new Guid("724d7b7b-a362-566b-b0a7-5088f219266f"), menu.Id);
        Assert.Equal(new Guid("27d00d31-a7ce-56d9-ab60-e2c50c02427d"), menu.Tabs[0].Id);
        Assert.Equal(new Guid("90598091-ecc6-50ab-bcb9-0941439c2e80"), menu.Tabs[0].Sets[0].Id);
    }

    [Fact]
    public void Station_list_and_item_side_keys_bind_to_the_recipes_they_lowered_to()
    {
        Forge(null);
        var rows = StationMenuCompiler.LiveBindings().OrderBy(b => b.Key, StringComparer.Ordinal).ToList();
        Assert.Equal(new[] { "bolts", "reclaim" }, rows.Select(b => b.Key));
        Assert.Equal("menu-example.bolts.recipe", rows[0].Name);
        Assert.Equal(RecipeId("menu-example", "bolts"), rows[0].Id);
        Assert.Equal("menu-example.forge.makes.Material_T1_Metal_Scraps", rows[1].Name);
        Assert.Equal(RecipeId("menu-example", "forge.makes.Material_T1_Metal_Scraps"), rows[1].Id);
        Assert.All(rows, b => Assert.Equal(BlockIds.Derive("menu-example", "workshop", "forge"), b.Workshop));
    }

    [Fact]
    public void Two_recipes_for_the_same_output_bind_by_their_distinct_keys()
    {
        var m = Ledger("same-output");
        Station(m, "kiln", new StationBlock
        {
            Name = "Kiln", Kind = StationKind.Forge, Processing = new ProcessingBlock(),
            Menu = new MenuBlock { Tabs = [Tab("all", Set("main", "two", "one"))] },
            Recipes =
            [
                new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = "one", Needs = { ["Material_T1_Wood_Logs"] = 1 }, Seconds = 6 },
                new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = "two", Needs = { ["Material_T1_Wood_Logs"] = 2 }, Seconds = 8 },
            ],
        });
        InGame();
        _host.Apply();
        var record = Assert.Single(_store.Layers.Values).Record;
        Assert.Contains($"[{{\"key\":\"same-output:two\",\"recipeId\":{RecipeId("same-output", "kiln.makes.Material_T1_Metal_Scraps.2")}}},{{\"key\":\"same-output:one\",\"recipeId\":{RecipeId("same-output", "kiln.makes.Material_T1_Metal_Scraps")}}}]", record);
    }

    // ---- what a menu key binds to, and what it never does -------------------------------------------------------------

    [Fact]
    public void A_duplicate_key_is_refused_with_both_recipe_names()
    {
        // in one station's list: at once
        var e = Assert.Throws<BlockError>(() => Station(Ledger("dup-list"), "forge", new StationBlock
        {
            Name = "Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock(),
            Recipes =
            [
                new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = "x", Needs = { ["Material_T1_Wood_Logs"] = 1 }, Seconds = 6 },
                new RecipeBlock { Output = "Material_T1_Wood_Logs", MenuKey = "x", Needs = { ["Material_T1_Twigs"] = 2 }, Seconds = 6 },
            ],
        }));
        Assert.Equal("recipes[1].menuKey", e.Field);
        Assert.Contains("dup-list.forge.makes.Material_T1_Metal_Scraps", e.Problem);
        // a station's list and an item's recipe: when the menu is resolved, nothing stored, both named
        var m = Ledger("dup-sides");
        var forge = Station(m, "forge", new StationBlock
        {
            Name = "Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock(),
            Menu = new MenuBlock { Tabs = [Tab("t", Set("s", "x"))] },
            Recipes = [new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = "x", Needs = { ["Material_T1_Wood_Logs"] = 1 }, Seconds = 6 }],
        });
        Compiler.Define(m, "bolts", new ItemBlock { Name = "Bolts", Recipe = new RecipeBlock { Station = "forge", MenuKey = "x", Needs = { ["Material_T1_Metal_Scraps"] = 2 }, Seconds = 4 } });
        InGame();
        _host.Apply();
        Assert.Empty(_store.Layers);
        var intent = StationMenuRuntime.Find(7, BlockIds.Derive("dup-sides", "menu-layout", "forge.menu"))!;
        Assert.Equal("refused", intent.State);
        Assert.Contains("dup-sides.bolts.recipe and dup-sides.forge.makes.Material_T1_Metal_Scraps", intent.Reason);
        Assert.Contains(m.Errors, x => x.Contains("dup-sides.bolts.recipe"));
        // and through StationMenus.Define
        ModContext.CurrentMod = Context("dup-sides", 7);
        var a = Assert.Throws<ArgumentException>(() => StationMenus.Define(forge, new StationMenuDefinition { Key = "other", Tabs = [Tab("t", Set("s", "x"))] }));
        Assert.Contains("dup-sides.bolts.recipe and dup-sides.forge.makes.Material_T1_Metal_Scraps", a.Message);
    }

    [Fact]
    public void A_recipe_is_never_found_by_its_output_name_or_generated_name()
    {
        var m = Ledger("no-fallback");
        var forge = Station(m, "forge", new StationBlock
        {
            Name = "Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock(),
            Recipes = [new RecipeBlock { Output = "Material_T1_Metal_Scraps", Needs = { ["Material_T1_Wood_Logs"] = 1 }, Seconds = 6 }],
        });
        InGame();
        ModContext.CurrentMod = Context("no-fallback", 7);
        foreach (var name in new[] { "forge.makes.Material_T1_Metal_Scraps", "Material_T1_Metal_Scraps", "no-fallback.forge.makes.Material_T1_Metal_Scraps" })
        {
            var e = Assert.Throws<ArgumentException>(() => StationMenus.Define(forge, new StationMenuDefinition { Key = "m", Tabs = [Tab("t", Set("s", name))] }));
            Assert.Contains("has the menuKey", e.Message);
        }
        Assert.Empty(_store.Layers);
    }

    [Fact]
    public void A_key_of_another_workshop_or_listed_twice_is_refused()
    {
        var m = Ledger("two-forges");
        var a = Station(m, "a", new StationBlock { Name = "A", Kind = StationKind.Forge, Processing = new ProcessingBlock() });
        Station(m, "b", new StationBlock
        {
            Name = "B", Kind = StationKind.Forge, Processing = new ProcessingBlock(),
            Recipes = [new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = "k", Needs = { ["Material_T1_Wood_Logs"] = 1 }, Seconds = 6 }],
        });
        InGame();
        ModContext.CurrentMod = Context("two-forges", 7);
        var e = Assert.Throws<ArgumentException>(() => StationMenus.Define(a, new StationMenuDefinition { Key = "m", Tabs = [Tab("t", Set("s", "k"))] }));
        Assert.Contains("another workshop", e.Message);
        var twice = Assert.Throws<ArgumentException>(() => StationMenus.Define(a, new StationMenuDefinition { Key = "m", Tabs = [Tab("t", Set("s", "k")), Tab("u", Set("s", "TWO-FORGES:k"))] }));
        Assert.Contains("listed twice", twice.Message);
        Assert.Equal("menu.tabs[0].sets[0].recipes[1]", Field(() => Forge([Tab("t", Set("s", "bolts", "MENU-twice:bolts"))], "menu-twice")));
    }

    [Fact]
    public void A_key_of_another_mod_binds_when_qualified()
    {
        // another mod names the station by "mod:name": the station's mod is in the process-wide registry of mods' blocks
        var host = TheBlocks.ModFor($"menu-host-{Guid.NewGuid():N}", () => 7);
        (host.ManifestClients, host.OwnedByMod) = ("mod", true);
        var (_, forge) = Forge(null, into: host);
        var other = Ledger("other-mod", 9);
        Compiler.Define(other, "wire", new ItemBlock { Name = "Wire", Recipe = new RecipeBlock { Station = $"{host.Id}:forge", MenuKey = "wire", Needs = { ["Material_T1_Metal_Scraps"] = 1 }, Seconds = 3 } });
        InGame();
        ModContext.CurrentMod = Context(host.Id, 7);
        var r = StationMenus.Define(forge, new StationMenuDefinition { Key = "winter", Tabs = [Tab("t", Set("s", "reclaim", "Other-Mod:wire"))] });
        Assert.True(r.Ok, r.Reason);
        Assert.Contains($"{{\"key\":\"other-mod:wire\",\"recipeId\":{RecipeId("other-mod", "wire")}}}", Assert.Single(_store.Layers.Values).Record);
    }

    // ---- the record's form --------------------------------------------------------------------------------------------

    [Fact]
    public void The_record_is_canonical_languages_in_ordinal_order_and_strings_as_nlohmann_writes_them()
    {
        var station = new StationMenuCompiler.Station("m", "s", BlockIds.Derive("m", "workshop", "s"), BlockIds.Fnv(BlockIds.Derive("m", "workshop", "s")), 0x4da88f73, true);
        string Record(UiText name)
        {
            var menu = StationMenuCompiler.Check("m", "s.menu", "m", [new StationMenuTabBlock { Key = "t", Name = name, Sets = [] }], "menu.");
            var res = StationMenuCompiler.Resolve(menu, station, [], _ => "registered", prune: false);
            Assert.Equal(StationMenuCompiler.Outcome.Record, res.Outcome);
            return res.Record;
        }
        var a = Record(Text(("en", "Metal"), ("de", "Metall"), ("pt-br", "Metal"), ("*", "Metal")));
        var b = Record(Text(("*", "Metal"), ("pt-br", "Metal"), ("de", "Metall"), ("en", "Metal")));
        Assert.Equal(a, b);
        Assert.Contains("\"name\":{\"*\":\"Metal\",\"de\":\"Metall\",\"en\":\"Metal\",\"pt-br\":\"Metal\"}", a);
        // a plain string is every language's; quote, backslash and control characters escaped, the rest raw
        var c = Record("Métal \"x\"\\\n\t\u0001\u007f€");
        Assert.Contains("\"name\":{\"*\":\"Métal \\\"x\\\"\\\\\\n\\t\\u0001\u007f€\"}", c);
        Assert.DoesNotContain(" ", c.Replace("Métal ", ""));  // compact
    }

    [Fact]
    public void A_menu_of_no_tab_or_a_tab_of_no_set_is_refused_and_one_empty_set_is_legal()
    {
        // review S2 (ruling): a menu needs a tab and a tab needs a set; the game's own data never has either shape empty
        var m = Ledger("empty-menu");
        Station(m, "forge", new StationBlock
        {
            Name = "Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock(),
            Menu = new MenuBlock { Tabs = [] },
        });
        _host.Apply();
        Assert.Empty(_store.Layers);
        Assert.Contains(m.Errors, x => x.Contains("at least one tab"));
        var forge = Station(Ledger("empty-sets"), "forge", new StationBlock { Name = "Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock() });
        ModContext.CurrentMod = Context("empty-sets", 7);
        var none = Assert.Throws<ArgumentException>(() => StationMenus.Define(forge, new StationMenuDefinition { Key = "m", Tabs = [] }));
        Assert.Contains("at least one tab", none.Message);
        var noSet = Assert.Throws<ArgumentException>(() => StationMenus.Define(forge, new StationMenuDefinition { Key = "m", Tabs = [Tab("a"), Tab("b", Set("s"))] }));
        Assert.Contains("at least one set", noSet.Message);
        // one tab with one empty set: a menu that lists nothing (client check 16a-c4)
        var r = StationMenus.Define(forge, new StationMenuDefinition { Key = "m", Tabs = [Tab("b", Set("s"))] });
        Assert.True(r.Ok, r.Reason);
        Assert.Contains("\"key\":\"s\",\"recipes\":[]}", _store.Layers[r.Registration!.Handle].Record);
    }

    [Fact]
    public void An_icon_is_the_nth_tab_of_the_menu_it_is_like_minus_one_the_first()
    {
        string Icon(int n)
        {
            var menu = StationMenuCompiler.Check("m", "s.menu", "m", [new StationMenuTabBlock { Key = "t", Name = "T", IconFromTab = n, Sets = [] }], "menu.");
            return menu.Tabs[0].Icon.ToString(System.Globalization.CultureInfo.InvariantCulture);
        }
        Assert.Equal(-1, new StationMenuTabBlock { Key = "t", Name = "T", Sets = [] }.IconFromTab);
        Assert.Equal("0", Icon(-1));
        Assert.Equal("3", Icon(3));
        Assert.Equal("14", Icon(14));
        foreach (var n in new[] { -2, 15 })
            Assert.Equal("menu.tabs[0].iconFromTab", Field(() => Forge([new StationMenuTabBlock { Key = "t", Name = "T", IconFromTab = n, Sets = [] }], $"icon{n + 10}")));
    }

    // ---- refusals -------------------------------------------------------------------------------------------------------

    [Fact]
    public void Tabs_on_a_game_workshop_are_refused()
    {
        Assert.Equal("menu.tabs", Field(() => Station(Ledger("game-ws"), "bench", new StationBlock { Name = "Bench", Kind = StationKind.Workbench, Menu = new MenuBlock { Tabs = [] } })));
        Assert.Equal("menu.tabs", Field(() => Station(Ledger("game-ws2"), "forge", new StationBlock
        {
            Name = "Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock(), OwnWorkshop = false, Menu = new MenuBlock { Tabs = [] },
        })));
        var shared = Station(Ledger("game-ws3"), "bench", new StationBlock { Name = "Bench", Kind = StationKind.Workbench });
        ModContext.CurrentMod = Context("game-ws3", 7);
        Assert.Throws<ArgumentException>(() => StationMenus.Define(shared, new StationMenuDefinition { Key = "m", Tabs = [] }));
        // a menuKey on a recipe that is not at a workshop of a station's own
        Assert.Equal("recipe.menuKey", Field(() => Compiler.Define(Ledger("hand-key"), "thing", new ItemBlock { Name = "Thing", Recipe = new RecipeBlock { MenuKey = "k", Needs = { ["Material_T1_Twigs"] = 1 } } })));
    }

    [Fact]
    public void Bad_keys_duplicates_headings_and_counts_are_refused_by_field()
    {
        var n = 0;
        string Bad(params StationMenuTabBlock[] tabs) => Field(() => Forge(tabs, $"bad{n++}"));
        Assert.Equal("menu.tabs[0].key", Bad(Tab("a b")));
        Assert.Equal("menu.tabs[0].key", Bad(Tab("a:b")));
        Assert.Equal("menu.tabs[0].key", Bad(Tab(new string('k', 65))));
        Assert.Equal("menu.tabs[1].key", Bad(Tab("a"), Tab("a")));
        Assert.Equal("menu.tabs[0].sets[1].key", Bad(Tab("a", Set("s"), Set("s"))));
        Assert.Equal("menu.tabs[0].sets[0].recipes[0]", Bad(Tab("a", Set("s", "a:b:c"))));
        Assert.Equal("menu.tabs[0].sets[0].heading", Bad(Tab("a", new StationMenuSetBlock { Key = "s", Heading = "H", Recipes = [] })));
        Assert.Equal("menu.tabs", Bad([.. Enumerable.Range(0, 16).Select(i => Tab($"t{i}"))]));
        Assert.Equal("menu.tabs[0].sets", Bad(Tab("a", [.. Enumerable.Range(0, 29).Select(i => Set($"s{i}"))])));
        Assert.Equal("menu.tabs[0].sets[1].recipes", Bad(Tab("a", Set("s", [.. Enumerable.Range(0, 300).Select(i => $"r{i}")]), Set("u", [.. Enumerable.Range(300, 213).Select(i => $"r{i}")]))));
        Assert.Equal("menu.tabs[0].name", Bad(new StationMenuTabBlock { Key = "a", Name = new UiText(Enumerable.Range(0, 36).ToDictionary(i => $"{(char)('a' + i / 26)}{(char)('a' + i % 26)}", _ => "x")), Sets = [] }));
        Assert.Equal("menu.tabs[0].name", Bad(new StationMenuTabBlock { Key = "a", Name = "", Sets = [] }));
        Assert.Equal("menu.tabs[0].name", Bad(new StationMenuTabBlock { Key = "a", Name = new string('x', 4097), Sets = [] }));
        Assert.Equal("menu.tabs[0].name", Bad(new StationMenuTabBlock { Key = "a", Name = Text(("english", "x")), Sets = [] }));
        Assert.Equal("menu.tabs[0].sets[1].heading", Bad(Tab("a", Set("s"), new StationMenuSetBlock { Key = "u", Heading = new string('é', 2049), Recipes = [] })));
        // the edges are fine
        Forge([.. Enumerable.Range(0, 15).Select(i => Tab($"t{i}", [.. Enumerable.Range(0, 28).Select(j => Set($"s{j}"))]))], "edges");
        Forge([new StationMenuTabBlock { Key = new string('k', 64), Name = new string('é', 2048), Sets = [Set("s", [.. Enumerable.Range(0, 512).Select(i => $"r{i}")])] }], "edges2");
        // a menuKey's own grammar
        Assert.Equal("recipes[0].menuKey", Field(() => Station(Ledger("key-grammar"), "forge", new StationBlock
        {
            Name = "Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock(),
            Recipes = [new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = "a:b", Needs = { ["Material_T1_Wood_Logs"] = 1 }, Seconds = 6 }],
        })));
        // StationMenus.Define: the same checks, as ArgumentException
        var forge = Station(Ledger("define-bad"), "forge", new StationBlock { Name = "Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock() });
        ModContext.CurrentMod = Context("define-bad", 7);
        Assert.Throws<ArgumentException>(() => StationMenus.Define(forge, new StationMenuDefinition { Key = "a b", Tabs = [] }));
        Assert.Contains("definition.tabs[0].sets[0].heading", Assert.Throws<ArgumentException>(() => StationMenus.Define(forge, new StationMenuDefinition { Key = "m", Tabs = [Tab("a", new StationMenuSetBlock { Key = "s", Heading = "H", Recipes = [] })] })).Message);
    }

    // ---- blocks JSON ---------------------------------------------------------------------------------------------------

    private const string StationJson = """
        { "name": "Rune Forge", "kind": "forge", "processing": {},
          "menu": { "like": "workbench", "tabs": [
            { "key": "metal", "name": { "en": "Metal", "de": "Metall" }, "iconFromTab": 0, "sets": [
              { "key": "main", "recipes": ["reclaim", "bolts"] },
              { "key": "fine", "heading": "Fine work", "recipes": ["other-mod:wire"] } ] } ] },
          "recipes": [ { "output": "Material_T1_Metal_Scraps", "menuKey": "reclaim", "needs": { "Material_T1_Wood_Logs": 1 }, "seconds": 6 } ] }
        """;

    [Fact]
    public void Blocks_json_with_menu_tabs_reads_as_the_records()
    {
        var s = BlockJson.Parse<StationBlock>(StationJson);
        var menu = s.Menu!.Inline!;
        var tab = Assert.Single(menu.Tabs!);
        Assert.Equal(("metal", 0, "Metall", "Metal"), (tab.Key, tab.IconFromTab, tab.Name.For("de"), tab.Name.For("en")));
        Assert.Equal(new[] { "main", "fine" }, tab.Sets.Select(x => x.Key));
        Assert.Null(tab.Sets[0].Heading);
        Assert.Equal("Fine work", tab.Sets[1].Heading!.For("*"));
        Assert.Equal(new[] { "reclaim", "bolts" }, tab.Sets[0].Recipes.Select(r => r.Value));
        Assert.Equal("other-mod:wire", tab.Sets[1].Recipes[0].Value);
        Assert.Equal("reclaim", s.Recipes![0].MenuKey);
        // what the reader refuses
        Assert.ThrowsAny<JsonException>(() => BlockJson.Parse<StationBlock>(StationJson.Replace("\"other-mod:wire\"", "\"a:b:c\"")));
        Assert.ThrowsAny<JsonException>(() => BlockJson.Parse<StationBlock>(StationJson.Replace("\"de\": \"Metall\"", "\"EN\": \"Metall\"")));
        Assert.ThrowsAny<JsonException>(() => BlockJson.Parse<StationBlock>(StationJson.Replace("\"iconFromTab\": 0,", "\"icon\": 0,")));
        Assert.ThrowsAny<JsonException>(() => BlockJson.Parse<StationBlock>(StationJson.Replace("\"key\": \"fine\", ", "")));
        // and it compiles: the station's own tabs, checked
        var (h, low, _) = Compiler.Lower(Ledger("json-menu"), "forge", s);
        Assert.Equal(1, (int)low.Json["menuTabs"]!["tabs"]!);
    }

    [Fact]
    public void The_schema_has_the_records_fields()
    {
        using var schema = JsonDocument.Parse(File.ReadAllText(SchemaFile()));
        var defs = schema.RootElement.GetProperty("$defs");
        string[] Keys(string def) => [.. defs.GetProperty(def).GetProperty("properties").EnumerateObject().Select(p => p.Name).Order(StringComparer.Ordinal)];
        string[] Fields(Type t) => [.. t.GetProperties().Where(p => p.SetMethod is { IsPublic: true }).Select(p => JsonNamingPolicy.CamelCase.ConvertName(p.Name)).Order(StringComparer.Ordinal)];
        Assert.Equal(Fields(typeof(StationMenuTabBlock)), Keys("stationMenuTab"));
        Assert.Equal(Fields(typeof(StationMenuSetBlock)), Keys("stationMenuSet"));
        Assert.Equal(Fields(typeof(MenuBlock)), Keys("menu"));
        Assert.Contains("menuKey", Keys("recipe"));
        Assert.Equal(-1, defs.GetProperty("stationMenuTab").GetProperty("properties").GetProperty("iconFromTab").GetProperty("minimum").GetInt32());
    }

    private static string SchemaFile([CallerFilePath] string source = "") =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(source)!, "../../..", "schema/kiln-blocks.schema.json"));

    [Fact]
    public void A_menu_key_changes_nothing_a_recipe_lowers_to()
    {
        JsonObject Lowered(string? key1, string? key2)
        {
            var m = new ModBlocks(GoldenMod, 1) { ManifestClients = "mod" };
            var (forge, low, _) = Compiler.Lower(m, "forge", new StationBlock
            {
                Name = "Rune Forge", Kind = StationKind.Forge, Processing = new ProcessingBlock { Slots = 6 }, Menu = new MenuBlock { Like = StationKind.Workbench },
                Recipes =
                [
                    new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = key1, Needs = { ["Material_T1_Wood_Logs"] = 1 }, Seconds = 6 },
                    new RecipeBlock { Output = "Material_T1_Metal_Scraps", MenuKey = key2, Needs = { ["Material_T1_Wood_Logs"] = 2 }, Seconds = 8 },
                ],
            });
            m.Handles["forge"] = forge;
            var item = Compiler.Lower(m, "bolts", new ItemBlock { Name = "Bolts", Recipe = new RecipeBlock { Station = "forge", MenuKey = key1 == null ? null : "bolts", Needs = { ["Material_T1_Metal_Scraps"] = 2 }, Seconds = 4 } }).Low.Json;
            return new JsonObject { ["forge"] = low.Json.DeepClone(), ["bolts"] = item.DeepClone() };
        }
        var plain = Lowered(null, null);
        Assert.Equal(ItemCompileGoldenTests.Text(plain["forge"]!), ItemCompileGoldenTests.Text(GoldenLowered("forge")));
        Assert.Equal(ItemCompileGoldenTests.Text(plain), ItemCompileGoldenTests.Text(Lowered("one", "two")));
    }

    // ---- at run time: pending content, upkeep, layers -------------------------------------------------------------------

    [Fact]
    public void A_recipe_not_in_the_game_yet_is_PendingContent_and_tried_again()
    {
        var (_, forge) = Forge(ExampleTabs);
        InGame("pending");
        _host.Apply();
        Assert.Empty(_store.Layers);
        var intent = StationMenuRuntime.Find(7, BlockIds.Derive("menu-example", "menu-layout", "forge.menu"))!;
        Assert.Equal("pending", intent.State);
        Assert.StartsWith("PendingContent", intent.Reason);
        ModContext.CurrentMod = Context("menu-example", 7);
        var r = StationMenus.Define(forge, new StationMenuDefinition { Key = "winter", Tabs = ExampleTabs });
        Assert.False(r.Ok);
        Assert.StartsWith("PendingContent", r.Reason);
        Assert.Null(r.Registration);
        Assert.Empty(_store.Layers);
        // registered a little later, in the same content generation: a ready tick between generations tries again
        InGame();
        for (var i = 0; i < 16; i++) _host.Tick();
        Assert.Equal(ExampleRecord, Assert.Single(_store.Layers.Values).Record);
    }

    [Fact]
    public void Each_content_generation_prunes_recipes_that_went_and_binds_a_key_that_returns_storing_only_changes()
    {
        Forge(ExampleTabs);
        InGame();
        _host.Apply();
        Assert.Equal(1ul, StationMenuStore.Generation);
        _host.Apply();  // the same content again: the same record, no new generation
        Assert.Equal(1ul, StationMenuStore.Generation);
        SetState("menu-example.bolts.recipe", "orphaned");
        _host.Apply();
        Assert.Equal(2ul, StationMenuStore.Generation);
        var layer = Assert.Single(_store.Layers.Values);
        Assert.Contains("menu-example:reclaim", layer.Record);
        Assert.DoesNotContain("menu-example:bolts", layer.Record);
        SetState("menu-example.bolts.recipe", "registered");
        _host.Apply();
        Assert.Equal(3ul, StationMenuStore.Generation);
        Assert.Equal(ExampleRecord, Assert.Single(_store.Layers.Values).Record);
        // the last recipe gone: the menu stays, its sets list nothing
        SetState("menu-example.bolts.recipe", "orphaned");
        SetState("menu-example.forge.makes.Material_T1_Metal_Scraps", "orphaned");
        _host.Apply();
        Assert.Contains("\"recipes\":[]", Assert.Single(_store.Layers.Values).Record);
    }

    [Fact]
    public void A_layer_over_the_stations_own_tabs_shows_and_its_dispose_reveals_them_again()
    {
        var (_, forge) = Forge(ExampleTabs);
        InGame();
        _host.Apply();
        var workshop = BlockIds.Fnv(BlockIds.Derive("menu-example", "workshop", "forge"));
        Assert.Equal(ExampleRecord, _store.Effective(workshop));
        // another mod lays its layer over it
        var other = Context("other-mod", 9);
        ModContext.CurrentMod = other;
        var winter = new StationMenuDefinition { Key = "winter", Tabs = [Tab("cold", Set("main", "menu-example:bolts"))] };
        var r = StationMenus.Define(forge, winter);
        Assert.True(r.Ok, r.Reason);
        Assert.Equal(2ul, r.Generation);
        Assert.Equal(BlockIds.Derive("other-mod", "menu-layout", "winter"), r.Registration!.DefinitionId);
        Assert.Contains("\"definition\":{\"id\":\"" + BlockIds.Derive("other-mod", "menu-layout", "winter") + "\",\"key\":\"winter\",\"mod\":\"other-mod\"}", _store.Effective(workshop));
        Assert.Contains("\"origin\":{\"mod\":\"menu-example\",\"stationKey\":\"forge\"", _store.Effective(workshop));
        // the same again: nothing changes, the same registration
        var again = StationMenus.Define(forge, winter);
        Assert.Same(r.Registration, again.Registration);
        Assert.Equal(2ul, again.Generation);
        Assert.Equal(2ul, r.Registration.Generation);
        // only its mod takes it off
        ModContext.CurrentMod = Context("menu-example", 7);
        Assert.Throws<ArgumentException>(() => r.Registration.Dispose());
        Assert.Contains("winter", _store.Effective(workshop));
        ModContext.CurrentMod = other;
        r.Registration.Dispose();
        Assert.Equal(ExampleRecord, _store.Effective(workshop));
        Assert.Equal(3ul, StationMenuStore.Generation);
        r.Registration.Dispose();  // a second time: nothing
        Assert.Equal(3ul, StationMenuStore.Generation);
        Assert.Single(_store.Layers);
    }

    [Fact]
    public void A_retired_or_reloaded_mod_takes_its_layers_with_it()
    {
        var (m, forge) = Forge(ExampleTabs);
        InGame();
        _host.Apply();
        var other = Context("other-mod", 9);
        ModContext.CurrentMod = other;
        Assert.True(StationMenus.Define(forge, new StationMenuDefinition { Key = "winter", Tabs = [] }).Ok);
        Assert.Equal(2, _store.Layers.Count);
        ModContext.CurrentMod = _outer;
        other.Retire();
        StationMenuRuntime.Pump();
        Assert.Equal(ExampleRecord, Assert.Single(_store.Layers.Values).Record);
        // the station's mod reloads: its ledger has another owner, the old layer goes
        m.Owner = 8;
        StationMenuRuntime.Pump();
        Assert.Empty(_store.Layers);
        Assert.Equal(0, StationMenuStore.Count);
    }

    [Fact]
    public void Without_the_store_Define_throws_naming_stationmenus_store()
    {
        var (_, forge) = Forge(null);
        _store.Available = false;
        ModContext.CurrentMod = Context("menu-example", 7);
        var e = Assert.Throws<KilnUnavailableException>(() => StationMenus.Define(forge, new StationMenuDefinition { Key = "m", Tabs = [] }));
        Assert.Equal("stationmenus.store", e.Capability);
    }

    [Fact]
    public void The_declarations_signal_comes_once_after_the_block_pass_and_the_first_resolution()
    {
        var told = 0;
        var passDone = false;
        StationMenuRuntime.Declared = () => told++;
        StationMenuRuntime.BlockPassDone = () => passDone;
        Forge(ExampleTabs);
        StationMenuRuntime.Pump();
        Assert.Equal(0, told);  // the block pass is not over
        passDone = true;
        StationMenuRuntime.Pump();
        Assert.Equal(0, told);  // the station's own tabs were not tried yet
        InGame("pending");
        _host.Apply();          // tried: PendingContent counts as tried
        StationMenuRuntime.Pump();
        StationMenuRuntime.Pump();
        Assert.Equal(1, told);
        Assert.True(StationMenuRuntime.IsDeclared);
    }

    [Fact]
    public void With_no_menus_the_declarations_signal_follows_the_block_pass()
    {
        var told = 0;
        StationMenuRuntime.Declared = () => told++;
        StationMenuRuntime.BlockPassDone = () => true;
        StationMenuRuntime.Pump();
        Assert.Equal(1, told);
    }
}

/// <summary>The content-ready pass, faked: nothing runs until the test says; only the station menus' pass is run (the other passes the
/// blocks hooked are left alone), with the recipes' states the test put in <see cref="Items"/>.</summary>
internal sealed class MenuReadyHost : IReadyHost
{
    public readonly List<ContentDefinition> Items = [];
    private readonly List<(Func<bool> Run, Func<bool> Again)> _hooked = [];

    public void OnReady(Func<bool> run, Func<bool> again) => _hooked.Add((run, again));

    private static bool Mine(Delegate d) => d.Method.DeclaringType == typeof(StationMenuRuntime);

    /// <summary>A content generation: the pass runs.</summary>
    public void Apply()
    {
        foreach (var (run, _) in _hooked.ToList())
            if (Mine(run)) run();
    }

    /// <summary>A ready tick in the same generation: the pass runs when it asks to.</summary>
    public void Tick()
    {
        foreach (var (run, again) in _hooked.ToList())
            if (Mine(run) && again()) run();
    }

    public void After(int steps, Action run) { }
    public bool ModLoaded(string mod) => true;
    public IReadOnlyList<GameDataDefinition> GameDataDefinitions() => [];
    public IReadOnlyList<ContentDefinition> ContentDefinitions() => Items;
    public IReadOnlyList<ContentDefinition> Placeholders() => [];
    public Guid? ResolveLate(LateRef r) => null;
    public void RegisterProgram(ulong owner, Guid guid, byte[] blob) { }
    public void ProgramsChanged() { }
    public IReadOnlyList<string> ApplyRules(ulong owner, string json, string source) => [];
    public void CheckBalancing() { }
}

/// <summary>Registrations go nowhere: a test commits blocks without a game.</summary>
internal sealed class NullSink : Lowered.ILoweredSink
{
    public void Program(ulong owner, Guid guid, byte[] blob) { }
    public void Clone(ulong owner, string section, JsonObject def) { }
    public void Item(ulong owner, ItemDefinition d) { }
    public void Recipe(ulong owner, RecipeDefinition d) { }
}

/// <summary>kiln.stationmenus as the header says it behaves, in memory: one layer per (owner, definition id); the same canonical bytes
/// again change nothing; a change replaces the layer under the same handle; per workshop the layer accepted last shows, and removing
/// it shows the one before; the generation goes up by one per change; another owner's remove is refused, a second remove is NotFound.</summary>
internal sealed class FakeMenuStore : IStationMenuBackend
{
    internal sealed record Layer(ulong Owner, Guid Definition, uint Workshop, string Record, ulong Sequence);

    public bool Available { get; set; } = true;
    public readonly Dictionary<ulong, Layer> Layers = new();
    private ulong _generation, _next = 1, _sequence;

    public (int Rc, ulong Handle, ulong Generation, string State, string Error) Define(ulong owner, string record)
    {
        Guid definition;
        uint workshop;
        try
        {
            using var doc = JsonDocument.Parse(record);
            var root = doc.RootElement;
            if (root.GetProperty("format").GetInt32() != 1) return (StationMenuNative.InvalidArgument, 0, _generation, "", "UnsupportedFormat");
            definition = Guid.Parse(root.GetProperty("definition").GetProperty("id").GetString()!);
            var origin = root.GetProperty("origin");
            workshop = origin.GetProperty("workshopId").GetUInt32();
            if (BlockIds.Fnv(Guid.Parse(origin.GetProperty("workshopGuid").GetString()!)) != workshop)
                return (StationMenuNative.InvalidArgument, 0, _generation, "", "workshopId is not FNV-1a of workshopGuid");
            foreach (var tab in root.GetProperty("tabs").EnumerateArray())
                if (tab.GetProperty("sets").GetArrayLength() > 0 && tab.GetProperty("sets")[0].GetProperty("heading").ValueKind != JsonValueKind.Null)
                    return (StationMenuNative.InvalidArgument, 0, _generation, "", "the first set has a heading");
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or FormatException or InvalidOperationException)
        {
            return (StationMenuNative.InvalidArgument, 0, _generation, "", e.Message);
        }
        foreach (var (handle, layer) in Layers)
        {
            if (layer.Owner != owner || layer.Definition != definition) continue;
            if (layer.Record == record) return (StationMenuNative.Ok, handle, _generation, "unchanged", "");
            Layers[handle] = layer with { Record = record, Workshop = workshop, Sequence = ++_sequence };
            return (StationMenuNative.Ok, handle, ++_generation, "replaced", "");
        }
        var h = _next++;
        Layers[h] = new Layer(owner, definition, workshop, record, ++_sequence);
        return (StationMenuNative.Ok, h, ++_generation, "defined", "");
    }

    public (int Rc, ulong Generation) Remove(ulong owner, ulong handle)
    {
        if (!Layers.TryGetValue(handle, out var layer)) return (StationMenuNative.NotFound, _generation);
        if (layer.Owner != owner) return (StationMenuNative.InvalidArgument, _generation);
        Layers.Remove(handle);
        return (StationMenuNative.Ok, ++_generation);
    }

    /// <summary>The layer that shows for a workshop: the one accepted last.</summary>
    public string? Effective(uint workshop) => Layers.Values.Where(l => l.Workshop == workshop).OrderByDescending(l => l.Sequence).Select(l => l.Record).FirstOrDefault();

    public string? Export() =>
        $"{{\"format\":1,\"generation\":{_generation},\"menus\":[{string.Join(",", Layers.Values.Select(l => l.Workshop).Distinct().Order().Select(w => Effective(w)))}]}}";

    public (int Rc, string Error) RemoteReplace(ulong owner, string snapshot) => (StationMenuNative.Ok, "");
    public int RemoteClear(ulong owner) => StationMenuNative.Ok;
    public int SetPending(bool on) => StationMenuNative.Ok;
    public string? DrainApplied() => "[]";
    public StationMenuNative.Info? Info() => null;
}
