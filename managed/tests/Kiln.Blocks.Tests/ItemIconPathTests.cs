// SPDX-License-Identifier: MIT
// An item's PNG icon (a look's, a worn look's, a prop's, a material's block item's) reaches kiln.content as the file in the mod's folder, whichever way the
// block came. A data file's blocks compile on a tick of Kiln's own, with no mod code running: kiln.content's managed side then had no mod to read a relative
// path against, and kiln.assets opened it against <game>/kiln. A boot mod's OnBoot definitions lower before any loaded mod lists the mod: they read against
// the folder the boot phase gives. The fake kiln.content table below keeps what Kiln.Content hands the native side.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

// The mod list, kiln.content's table and kiln.worldtables' answer are process-wide: nothing else runs meanwhile.
[CollectionDefinition("item icons", DisableParallelization = true)]
public sealed class ItemIconCollection;

[Collection("item icons")]
public sealed unsafe class ItemIconPathTests : IDisposable
{
    private const string Id = "icon-path-test", Other = "icon-path-other";
    private const string Model = "19796b32-33cb-507e-827a-b2c23f720126";
    private static readonly byte[] Png = Convert.FromBase64String("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGNgAAIAAAUAAXpeqz8AAAAASUVORK5CYII=");

    // what kiln.content's native side was given: one item definition per call
    private static readonly List<JsonObject> Sent = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Define(ulong owner, uint kind, NativeStr json, byte* report, nuint cap, nuint* len)
    {
        if (kind == (uint)ContentKind.Items) Sent.Add(JsonNode.Parse(json.ToString())!.AsObject());
        if (cap > 0) report[0] = 0;
        *len = 0;
        return ContentNative.Ok;
    }

    private sealed class FakeMods(params ModEntry[] mods) : IModsBackend
    {
        public IReadOnlyList<ModEntry> All => mods;
        public ModSide Side => ModSide.Server;
        public ModEntry? Get(string id) => mods.FirstOrDefault(m => m.Id == id);
        public ModStats Stats(string id) => default;
        public bool Send(ModContext? sender, string targetId, string method, object? payload) => false;
        public Task<ModOperationResult> Request(ModOperation op, string arg) => throw new NotSupportedException();
        public void Request(ModOperation op, string arg, Action<ModOperationResult> done, ModContext? caller) => throw new NotSupportedException();
    }

    private readonly string _root, _dir, _other;
    private readonly IModsBackend? _mods;
    private readonly ModContext? _caller;
    private readonly FieldInfo _tableField = typeof(Content).GetField("_table", BindingFlags.Static | BindingFlags.NonPublic)!;
    private readonly object? _table;
    private readonly ContentNative.Table* _fake;

    public ItemIconPathTests()
    {
        _root = Directory.CreateTempSubdirectory("kiln-item-icon-").FullName;
        _dir = Path.Combine(_root, "mods", Id);
        _other = Path.Combine(_root, "mods", Other);
        Directory.CreateDirectory(Path.Combine(_dir, "icons"));
        Directory.CreateDirectory(Path.Combine(_dir, "blocks"));
        Directory.CreateDirectory(Path.Combine(_other, "icons"));
        foreach (var n in new[] { "cinderbrand", "gem", "helm", "hood", "crop", "trim", "stool", "brick", "loam" })
            File.WriteAllBytes(Path.Combine(_dir, "icons", n + ".png"), Png);
        File.WriteAllBytes(Path.Combine(_other, "icons", "cape.png"), Png);
        File.WriteAllBytes(Path.Combine(_root, "outside.png"), Png);
        _mods = ModsBackend.Current;
        ModsBackend.Current = new FakeMods(
            new ModEntry(Id, Id, "1.0.0", _dir, ModSide.Both, ModState.Loaded, null, [], 0, null),
            new ModEntry(Other, Other, "1.0.0", _other, ModSide.Both, ModState.Loaded, null, [], 1, null));
        _caller = HostAccess.Current;
        HostAccess.Current = null;   // a data file's blocks compile on Kiln's tick: no mod's code is running
        _table = _tableField.GetValue(null);
        _fake = (ContentNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(ContentNative.Table));
        _fake->Header.Size = (uint)sizeof(ContentNative.Table);
        _fake->Header.Version = 4;
        _fake->Define = &Define;
        _tableField.SetValue(null, Pointer.Box(_fake, typeof(ContentNative.Table*)));
        Sent.Clear();
    }

    public void Dispose()
    {
        _tableField.SetValue(null, _table);
        NativeMemory.Free(_fake);
        HostAccess.Current = _caller;
        ModsBackend.Current = _mods;
        WorldTablesNative.TestDefine = null;
        Directory.Delete(_root, true);
    }

    private string Full(string rel) => Path.GetFullPath(Path.Combine(_dir, rel));

    private static ModBlocks NewMod(string id = Id) => new(id, 7) { ManifestClients = "mod" };

    private static string? IconSent(string item) => (string?)Sent.SingleOrDefault(j => (string?)j["name"] == item)?["icon"];

    private ModBlocks Load(string json)
    {
        File.WriteAllText(Path.Combine(_dir, "blocks", "icons.json"), json);
        var mod = NewMod();
        BlockJson.LoadMod(mod, _dir);
        return mod;
    }

    [Fact]
    public void A_data_files_png_icons_reach_kiln_content_as_the_files_in_the_mods_folder()
    {
        var mod = Load($$"""
            { "format": 1,
              "weapons": { "cinderbrand": { "name": "Cinderbrand", "class": "sword", "stats": { "level": 45, "power": 92, "rarity": "legendary" },
                                            "visual": { "model": "{{Model}}", "icon": "icons/cinderbrand.png" } } },
              "items": { "gem": { "name": "Gem", "visual": { "icon": "icons/gem.png" } } },
              "armor": { "helm": { "name": "Helm", "slot": "head", "stats": { "level": 5, "physical": 10, "magical": 5 },
                                   "look": { "like": "Vanity_TX_Hunter_Helmet", "icon": "icons/helm.png" } } },
              "cosmetics": { "hood": { "name": "Hood", "slot": "head", "look": { "like": "Vanity_TX_Hunter_Helmet", "icon": "icons/hood.png" } } },
              "hair": { "crop": { "name": "Crop", "look": { "like": "VC_Hair_04_02_frenchcrop", "icon": "icons/crop.png" } } },
              "beards": { "trim": { "name": "Trim", "look": { "like": "VC_Beard12_stubble", "icon": "icons/trim.png" } } } }
            """);
        Assert.Empty(mod.Errors);
        foreach (var (item, file) in new[] { ("cinderbrand", "cinderbrand"), ("gem", "gem"), ("helm", "helm"), ("hood", "hood"), ("crop", "crop"), ("trim", "trim") })
            Assert.Equal(Full($"icons/{file}.png"), IconSent($"{Id}.{item}"));
    }

    [Fact]
    public void A_prop_and_a_materials_block_item_name_the_file_in_the_mods_folder()
    {
        var mod = NewMod();
        var prop = Compiler.Lower(mod, "stool", BlockJson.Parse<PropBlock>("""{ "name": "Stool", "visual": { "model": "item:Prop_Decoration_Comfort1_T0_Table_Medium", "icon": "icons/stool.png" } }""")).Low.Json;
        Assert.Equal(Full("icons/stool.png"), (string?)prop["itemParts"]?["icon"]);
        static string Registered(string name, int id) => new JsonObject { ["blocks"] = new JsonArray(new JsonObject { ["name"] = name, ["status"] = "registered", ["id"] = id }) }.ToJsonString();
        WorldTablesNative.TestDefine = d => Registered(d.Contains("\"brick\"") ? "brick" : "loam", 40);
        var brick = Compiler.Lower(mod, "brick", BlockJson.Parse<BuildingMaterialBlock>("""{ "name": "Brick", "icon": "icons/brick.png" }""")).Low.Json;
        Assert.Equal(Full("icons/brick.png"), (string?)brick["itemParts"]?["icon"]);
        var loam = Compiler.Lower(mod, "loam", BlockJson.Parse<TerrainMaterialBlock>("""{ "name": "Loam", "placeable": true, "icon": "icons/loam.png" }""")).Low.Json;
        Assert.Equal(Full("icons/loam.png"), (string?)loam["itemParts"]?["icon"]);
        // A wrong optional picture is refused, not the material or its block item.
        var asked = 0;
        WorldTablesNative.TestDefine = d => { asked++; return Registered("clay", 41); };
        var building = Compiler.Lower(mod, "clay", BlockJson.Parse<BuildingMaterialBlock>("""{ "name": "Clay", "icon": "../clay.png" }""")).Low.Json;
        var terrain = Compiler.Lower(mod, "clay", BlockJson.Parse<TerrainMaterialBlock>("""{ "name": "Clay", "placeable": true, "icon": "icons/clay.png" }""")).Low.Json;
        Assert.Null(building["itemParts"]?["icon"]);
        Assert.Null(terrain["itemParts"]?["icon"]);
        Assert.Equal(2, asked);
        Assert.Equal(2, mod.Errors.Count);
    }

    [Fact]
    public void A_worn_look_of_another_mod_names_that_mods_file()
    {
        // Blocks.ModFor: in this assembly "Kiln.Blocks" names the test namespace
        var registry = typeof(ModBlocks).Assembly.GetType("Kiln.Blocks", throwOnError: true)!.GetMethod("ModFor", BindingFlags.Static | BindingFlags.NonPublic)!;
        var other = (ModBlocks)registry.Invoke(null, [Other, (Func<ulong>)(() => 8)])!;
        other.ManifestClients = "mod";
        Compiler.Define(other, "cape-look", new WornLook { Like = "Vanity_TX_Hunter_Helmet", Icon = "icons/cape.png" });
        Compiler.Define(NewMod(), "cape", new CosmeticBlock { Name = "Cape", Slot = CosmeticSlot.Head, Look = $"{Other}:cape-look" });
        Assert.Equal(Path.GetFullPath(Path.Combine(_other, "icons/cape.png")), IconSent($"{Id}.cape"));
    }

    [Fact]
    public void A_visual_look_owned_by_another_mod_uses_its_owners_icon_for_items_and_props()
    {
        var registry = typeof(ModBlocks).Assembly.GetType("Kiln.Blocks", throwOnError: true)!.GetMethod("ModFor", BindingFlags.Static | BindingFlags.NonPublic)!;
        var other = (ModBlocks)registry.Invoke(null, [Other, (Func<ulong>)(() => 8)])!;
        other.ManifestClients = "mod";
        Compiler.Define(other, "shared-visual", new VisualBlock { Model = "item:Prop_Decoration_Comfort1_T0_Table_Medium", Icon = "icons/cape.png" });
        var mod = NewMod();
        Compiler.Define(mod, "shared-item", new ItemBlock { Name = "Shared", Visual = $"{Other}:shared-visual" });
        Assert.Equal(Path.Combine(_other, "icons", "cape.png"), IconSent($"{Id}.shared-item"));
        var prop = Compiler.Lower(mod, "shared-prop", new PropBlock { Name = "Shared prop", Visual = $"{Other}:shared-visual" }).Low.Json;
        Assert.Equal(Path.Combine(_other, "icons", "cape.png"), (string?)prop["itemParts"]?["icon"]);
        Assert.Empty(mod.Errors);
    }

    [Fact]
    public void Mod_code_gives_a_path_relative_to_its_folder_or_the_full_path_of_a_file_there()
    {
        var context = HostAccess.CreateContext(new ModInfo(Id, Id, "1.0.0", _dir), 7, _dir);
        try
        {
            HostAccess.Current = context;
            Compiler.Define(NewMod(), "relative", new ItemBlock { Name = "Relative", Visual = new VisualBlock { Icon = "icons/gem.png" } });
            Compiler.Define(NewMod(), "full", new ItemBlock { Name = "Full", Visual = new VisualBlock { Icon = Path.Combine(_dir, "icons", "gem.png") } });
        }
        finally
        {
            HostAccess.Current = null;
            context.Retire();
        }
        Assert.Equal(Full("icons/gem.png"), IconSent($"{Id}.relative"));
        Assert.Equal(Full("icons/gem.png"), IconSent($"{Id}.full"));
    }

    [Fact]
    public void Mod_code_resolves_relative_PNGs_at_all_six_compile_sites()
    {
        var context = HostAccess.CreateContext(new ModInfo(Id, Id, "1.0.0", _dir), 7, _dir);
        var mod = NewMod();
        WorldTablesNative.TestDefine = d => new JsonObject { ["blocks"] = new JsonArray(new JsonObject
            { ["name"] = d.Contains("\"brick\"") ? "brick" : "loam", ["status"] = "registered", ["id"] = 40 }) }.ToJsonString();
        try
        {
            HostAccess.Current = context;
            (string Name, IBlock Block, string File)[] cases =
            [
                ("gem", new ItemBlock { Name = "Gem", Visual = new VisualBlock { Icon = "icons/gem.png" } }, "gem"),
                ("hood", new CosmeticBlock { Name = "Hood", Slot = CosmeticSlot.Head, Look = new WornLook { Like = "Vanity_TX_Hunter_Helmet", Icon = "icons/hood.png" } }, "hood"),
                ("crop", new HairBlock { Name = "Crop", Look = new WornLook { Like = "VC_Hair_04_02_frenchcrop", Icon = "icons/crop.png" } }, "crop"),
                ("trim", new BeardBlock { Name = "Trim", Look = new WornLook { Like = "VC_Beard12_stubble", Icon = "icons/trim.png" } }, "trim"),
                ("stool", new PropBlock { Name = "Stool", Visual = new VisualBlock { Model = "item:Prop_Decoration_Comfort1_T0_Table_Medium", Icon = "icons/stool.png" } }, "stool"),
                ("brick", new BuildingMaterialBlock { Name = "Brick", Icon = "icons/brick.png" }, "brick"),
                ("loam", new TerrainMaterialBlock { Name = "Loam", Placeable = true, Icon = "icons/loam.png" }, "loam"),
            ];
            foreach (var (name, block, file) in cases)
            {
                var low = Compiler.Lower(mod, name, block).Low.Json;
                Assert.Equal(Full($"icons/{file}.png"), (string?)low["itemParts"]?["icon"]);
            }
            Assert.Empty(mod.Errors);
        }
        finally
        {
            HostAccess.Current = null;
            context.Retire();
        }
    }

    [Theory]
    [InlineData(@"\\server\share\outside.png")]
    [InlineData("//server/share/outside.png")]
    [InlineData(@"\\?\C:\outside.png")]
    [InlineData("C:/outside.png")]
    [InlineData("C:outside.png")]
    [InlineData("icons/gem.png:stream.png")]
    [InlineData(@"\icons\gem.png")]
    [InlineData("icons/../icons/gem.png")]
    [InlineData("icons/stream:part.png")]
    public void Rooted_drive_stream_and_mixed_separator_paths_refuse_only_the_icon(string icon)
    {
        var json = new JsonObject { ["format"] = 1, ["items"] = new JsonObject { ["gem"] = new JsonObject
            { ["name"] = "Gem", ["visual"] = new JsonObject { ["icon"] = icon } } } }.ToJsonString();
        var mod = Load(json);
        Assert.Single(Sent);
        Assert.Null(IconSent($"{Id}.gem"));
        Assert.True(mod.Handles.ContainsKey("gem"));
        var error = Assert.Single(mod.Errors);
        Assert.Contains(", visual.icon:", error);
        Assert.Contains("the base icon stays", error);
    }

    [Fact]
    public void A_very_long_path_refuses_only_the_icon()
    {
        var icon = "icons/" + new string('a', 5000) + ".png";
        var json = new JsonObject { ["format"] = 1, ["items"] = new JsonObject { ["gem"] = new JsonObject
            { ["name"] = "Gem", ["visual"] = new JsonObject { ["icon"] = icon } } } }.ToJsonString();
        var mod = Load(json);
        Assert.Single(Sent);
        Assert.Null(IconSent($"{Id}.gem"));
        Assert.True(mod.Handles.ContainsKey("gem"));
        var error = Assert.Single(mod.Errors);
        Assert.Contains(", visual.icon:", error);
        Assert.Contains(icon, error);
        Assert.Contains("the base icon stays", error);
    }

    [Theory]
    [InlineData("icons/missing.png")]
    [InlineData("../../outside.png")]
    public void An_unusable_optional_icon_keeps_every_item_and_material_defined(string icon)
    {
        var json = $$"""
            { "format": 1,
              "items": { "gem": { "name": "Gem", "visual": { "icon": "{{icon}}" } } },
              "cosmetics": { "hood": { "name": "Hood", "slot": "head", "look": { "like": "Vanity_TX_Hunter_Helmet", "icon": "{{icon}}" } } },
              "hair": { "crop": { "name": "Crop", "look": { "like": "VC_Hair_04_02_frenchcrop", "icon": "{{icon}}" } } },
              "beards": { "trim": { "name": "Trim", "look": { "like": "VC_Beard12_stubble", "icon": "{{icon}}" } } },
              "buildingMaterials": { "brick": { "name": "Brick", "icon": "{{icon}}" } },
              "terrainMaterials": { "loam": { "name": "Loam", "placeable": true, "icon": "{{icon}}" } } }
            """;
        WorldTablesNative.TestDefine = d => new JsonObject { ["blocks"] = new JsonArray(new JsonObject
            { ["name"] = d.Contains("\"brick\"") ? "brick" : "loam", ["status"] = "registered", ["id"] = 40 }) }.ToJsonString();
        var mod = Load(json);
        foreach (var name in new[] { "gem", "hood", "crop", "trim", "brick", "loam" })
        {
            Assert.True(mod.Handles.ContainsKey(name), $"{name} disappeared: {string.Join("; ", mod.Errors)}");
            Assert.Null(IconSent($"{Id}.{name}"));
            var error = Assert.Single(mod.Errors, e => e.Contains($" {name}, ", StringComparison.Ordinal));
            Assert.Contains(name is "brick" or "loam" ? ", icon:" : name is "gem" or "stool" ? ", visual.icon:" : ", look.icon:", error);
            Assert.Contains(Full(icon), error);
            Assert.Contains("the base icon stays", error);
        }
        Assert.Equal(6, Sent.Count);
        // A prop also needs kiln.gamedata to commit its placed template, which this harness does not fake.
        var prop = Compiler.Lower(mod, "stool", BlockJson.Parse<PropBlock>($$"""{ "name": "Stool", "visual": { "model": "item:Prop_Decoration_Comfort1_T0_Table_Medium", "icon": "{{icon}}" } }"""));
        Assert.Equal($"{Id}.stool", (string?)prop.Low.Json["items"]?[0]?["name"]);
        Assert.Null(prop.Low.Json["itemParts"]?["icon"]);
        var propError = Assert.Single(mod.Errors, e => e.Contains(" stool, ", StringComparison.Ordinal));
        Assert.Contains(", visual.icon:", propError);
        Assert.Contains(Full(icon), propError);
        Assert.Contains("the base icon stays", propError);
        Assert.Equal(7, mod.Errors.Count);
    }

    [Theory]
    [InlineData(false, "icons/missing.png")]
    [InlineData(false, "../../outside.png")]
    [InlineData(false, "icons/broken.png")]
    [InlineData(true, "icons/missing.png")]
    [InlineData(true, "../../outside.png")]
    [InlineData(true, "icons/broken.png")]
    public void A_status_and_perk_keep_their_starters_icon_when_the_optional_png_is_unusable(bool isPerk, string icon)
    {
        File.WriteAllText(Full("icons/broken.png"), "not a PNG");
        var mod = NewMod();
        var low = isPerk
            ? Compiler.Lower(mod, "strong", BlockJson.Parse<PerkBlock>($$"""{ "name": "Strong", "modifies": { "max-health": 10 }, "icon": "{{icon}}" }""")).Low.Json
            : Compiler.Lower(mod, "warm", new StatusBlock { Name = "Warm", Seconds = 5, Icon = icon }).Low.Json;
        Assert.Equal($"{Id}.{(isPerk ? "strong" : "warm")}", (string?)low[isPerk ? "perks" : "buffs"]?[0]?["name"]);
        Assert.Null(low["icon"]);
        Assert.Empty(mod.OwnIcons);
        var error = Assert.Single(mod.Errors);
        Assert.Contains(", icon:", error);
        Assert.Contains(Full(icon), error);
        Assert.Contains("the base icon stays", error);
    }

    [Fact]
    public void A_path_that_leaves_the_mods_folder_or_names_no_file_is_refused_with_the_field()
    {
        File.CreateSymbolicLink(Path.Combine(_dir, "icons", "link.png"), Path.Combine(_root, "outside.png"));
        var outside = Path.Combine(_root, "outside.png").Replace("\\", "\\\\");
        var mod = Load($$"""
            { "format": 1,
              "items": {
                "up": { "name": "Up", "visual": { "icon": "../../outside.png" } },
                "rooted": { "name": "Rooted", "visual": { "icon": "{{outside}}" } },
                "linked": { "name": "Linked", "visual": { "icon": "icons/link.png" } },
                "missing": { "name": "Missing", "visual": { "icon": "icons/missing.png" } } },
              "cosmetics": { "worn-up": { "name": "Worn", "slot": "head", "look": { "like": "Vanity_TX_Hunter_Helmet", "icon": "../../outside.png" } } } }
            """);
        Assert.Equal(5, Sent.Count);
        Assert.All(Sent, item => Assert.Null(item["icon"]));
        Assert.Equal(5, mod.Handles.Count);
        string Error(string block) => Assert.Single(mod.Errors, e => e.Contains($" {block}, ", StringComparison.Ordinal));
        Assert.Contains("\"../../outside.png\": a file in the mod's folder, with a relative path", Error("up"));
        Assert.Contains("visual.icon:", Error("rooted"));
        Assert.Contains("a file in the mod's folder", Error("rooted"));
        Assert.Contains("links cannot leave it", Error("linked"));
        Assert.Contains($"\"icons/missing.png\" is not in the mod's folder ({Full("icons/missing.png")})", Error("missing"));
        Assert.Contains("look.icon:", Error("worn-up"));
        Assert.Contains("\"../../outside.png\": a file in the mod's folder", Error("worn-up"));
        Assert.All(mod.Errors, error => Assert.Contains("the base icon stays", error));
        Assert.Equal(5, mod.Errors.Count);
    }

    // ---- a boot mod's OnBoot definitions (BootLowering.LowerOwner): they lower before any loaded mod lists the mod, against the folder the boot
    // phase gives ("dir"); a file they name is the same file, refused for the same reasons, as when the loaded mod defines the same block

    private const string Boot = "icon-path-boot";

    private (string Dir, List<BootStaged> Staged) BootMod(Action<BootDefinitions> stage, string? folder = null)
    {
        var dir = folder ?? Path.Combine(_root, "mods", Boot);
        Directory.CreateDirectory(Path.Combine(dir, "icons"));
        File.WriteAllBytes(Path.Combine(dir, "icons", "token.png"), Png);
        var frame = new BootContext(Boot, 1, BootRoles.DedicatedServer, new UnavailableGameData());
        stage(BootDefinitions.For(frame));
        return (dir, frame.Staged);
    }

    private static JsonObject LowerBoot(string dir, IEnumerable<BootStaged> staged)
    {
        var r = JsonNode.Parse(BootLowering.LowerOwner(new JsonObject
        {
            ["id"] = Boot, ["clients"] = "mod", ["dir"] = dir,
            ["staged"] = new JsonArray(staged.Select(s => (JsonNode)new JsonObject { ["kind"] = s.Kind, ["key"] = s.Key, ["json"] = s.Json }).ToArray()),
        }.ToJsonString()))!.AsObject();
        Assert.True(r["ok"]!.GetValue<bool>(), (string?)r["reason"]);
        return r;
    }

    private static string? CommittedIcon(JsonObject lowered, string item) =>
        (string?)lowered["docs"]!["content"]!.AsArray().Single(d => (string?)d!["document"]!["definition"]!["name"] == $"{Boot}.{item}")!["document"]!["definition"]!["icon"];

    // the boot mod loaded, as at its runtime activation
    private static void Loaded(string dir) =>
        ModsBackend.Current = new FakeMods(new ModEntry(Boot, Boot, "1.0.0", dir, ModSide.Both, ModState.Loaded, null, [], 0, null));

    [Theory]
    [InlineData("icons/bad./token.png")]
    [InlineData("icons/bad /token.png")]
    [InlineData("/outside.png")]
    public void Boot_and_runtime_refuse_the_same_nonportable_icon(string icon)
    {
        var block = new ItemBlock { Name = "Token", Visual = new VisualBlock { Icon = icon } };
        var (dir, staged) = BootMod(d => Assert.True(d.Define("token", block).Ok));
        Assert.Null(CommittedIcon(LowerBoot(dir, staged), "token"));
        Loaded(dir);
        var mod = NewMod(Boot);
        Compiler.Define(mod, "token", block);
        Assert.Null(IconSent($"{Boot}.token"));
        Assert.Single(mod.Errors);
    }

    [Fact]
    public void A_failed_lowering_restores_runtime_icon_diagnostics()
    {
        var (dir, staged) = BootMod(d => Assert.True(d.Define("token", new ItemBlock { Name = "Token", Stack = -1 }).Ok));
        var request = new JsonObject { ["id"] = Boot, ["dir"] = dir, ["clients"] = "mod",
            ["staged"] = new JsonArray(staged.Select(x => (JsonNode)new JsonObject
                { ["kind"] = x.Kind, ["key"] = x.Key, ["json"] = x.Json }).ToArray()) };
        Assert.False(JsonNode.Parse(BootLowering.LowerOwner(request.ToJsonString()))!["ok"]!.GetValue<bool>());
        Assert.False(Lowered._bootLowering);
        Assert.Null(Lowered._sinkOverride);
        var mod = NewMod();
        Compiler.Lower(mod, "missing", new ItemBlock { Name = "Missing", Visual = new VisualBlock { Icon = "icons/missing.png" } });
        Assert.Single(mod.Errors);
    }

    [Fact]
    public void A_boot_icon_refusal_is_retained_even_without_runtime_activation()
    {
        var (dir, staged) = BootMod(d => Assert.True(d.Define("token", new ItemBlock
            { Name = "Token", Visual = new VisualBlock { Icon = "icons/missing.png" } }).Ok));
        var issue = Assert.Single(LowerBoot(dir, staged)["issues"]!.AsArray());
        Assert.Equal("IconRefused", (string?)issue!["code"]);
        Assert.Contains("the base icon stays", (string?)issue["reason"]);
        Assert.False(Lowered._bootLowering);
    }

    [Fact]
    public void An_unknown_mod_folder_never_commits_a_relative_icon()
    {
        var block = new ItemBlock { Name = "Token", Visual = new VisualBlock { Icon = "icons/token.png" } };
        var (_, staged) = BootMod(d => Assert.True(d.Define("token", block).Ok));
        Assert.Null(CommittedIcon(LowerBoot("", staged), "token"));
        var mod = NewMod(Boot);
        Compiler.Define(mod, "token", block);
        Assert.Null(IconSent($"{Boot}.token"));
        Assert.Single(mod.Errors);
    }

    [Theory]
    [InlineData("../../outside.png")]
    [InlineData("icons/link.png")]
    [InlineData("icons/missing.png")]
    public void A_boot_talent_refuses_an_icon_outside_its_folder_or_missing(string icon)
    {
        var json = new JsonObject { ["name"] = "Talent", ["icon"] = icon,
            ["modifies"] = new JsonObject { ["max-health"] = 10 },
            ["game"] = new JsonObject { ["from"] = new JsonArray("0x8efcc472"), ["offset"] = new JsonArray(170, 0) } }.ToJsonString();
        var (dir, staged) = BootMod(d => Assert.True(d.DefineJson("talents", "talent", json).Ok));
        File.CreateSymbolicLink(Path.Combine(dir, "icons", "link.png"), Path.Combine(_root, "outside.png"));
        var node = Assert.Single(LowerBoot(dir, staged)["docs"]!["skills"]!.AsArray())!["document"]!["nodes"]![0]!;
        Assert.Null(node["iconFile"]);
        Loaded(dir);
        var mod = NewMod(Boot);
        var low = Compiler.Lower(mod, "talent", BlockJson.Parse<TalentBlock>(json));
        Assert.Null(low.Low.Json["node"]!["iconFile"]);
        Assert.Single(mod.Errors);
    }

    [Fact]
    public void A_boot_staged_items_png_icon_is_committed_as_the_file_a_run_time_define_names()
    {
        var block = new ItemBlock { Name = "Boot Token", Visual = new VisualBlock { Icon = "icons/token.png" } };
        var (dir, staged) = BootMod(d => Assert.True(d.Define("wood-token", block).Ok));
        var committed = CommittedIcon(LowerBoot(dir, staged), "wood-token");   // the boot phase: no loaded mod lists it yet
        Loaded(dir);
        Compiler.Define(NewMod(Boot), "wood-token", block);
        var runtime = IconSent($"{Boot}.wood-token");
        Assert.Equal(Path.GetFullPath(Path.Combine(dir, "icons/token.png")), runtime);
        Assert.Equal(runtime, committed);
    }

    [Fact]
    public void A_boot_mods_folder_behind_a_link_and_icons_that_leave_it_or_are_missing_resolve_as_at_run_time()
    {
        var library = Path.Combine(_root, "library");
        var real = Path.Combine(library, Boot);
        string[] names = ["linked", "up", "missing", "escape"];
        string[] icons = ["icons/token.png", "../../outside.png", "icons/missing.png", "icons/link.png"];
        ItemBlock Block(int i) => new() { Name = names[i], Visual = new VisualBlock { Icon = icons[i] } };
        var (_, staged) = BootMod(d =>
        {
            for (var i = 0; i < names.Length; i++) Assert.True(d.Define(names[i], Block(i)).Ok);
        }, real);
        File.CreateSymbolicLink(Path.Combine(real, "icons", "link.png"), Path.Combine(_root, "outside.png"));
        var mods = Path.Combine(_root, "linked-mods");
        Directory.CreateSymbolicLink(mods, library);
        var dir = Path.Combine(mods, Boot);   // the folder as the boot phase names it: through the link
        var lowered = LowerBoot(dir, staged);
        Assert.Equal(names.Select(n => $"items/{n}"), lowered["definitions"]!.AsArray().Select(x => (string)x!));   // an icon never keeps the item out
        Assert.Equal(Path.Combine(real, "icons", "token.png"), CommittedIcon(lowered, "linked"));                     // link-resolved, as BlockFiles.InMod names it
        Loaded(dir);
        var mod = NewMod(Boot);
        for (var i = 0; i < names.Length; i++)
        {
            Compiler.Define(mod, names[i], Block(i));
            Assert.Equal(IconSent($"{Boot}.{names[i]}"), CommittedIcon(lowered, names[i]));
        }
        Assert.Null(CommittedIcon(lowered, "up"));
        Assert.Null(CommittedIcon(lowered, "missing"));
        Assert.Null(CommittedIcon(lowered, "escape"));
        Assert.Equal(3, mod.Errors.Count);   // the run-time define says why, once (the boot lowering retains an issue without logging; activation logs it)
    }

    [Fact]
    public void A_boot_staged_game_tree_talents_png_is_the_file_in_the_mods_folder_as_at_run_time()
    {
        const string json = """{ "name": "Boot Talent", "icon": "icons/token.png", "modifies": { "max-health": 10 }, "game": { "from": ["0x8efcc472"], "offset": [170, 0] } }""";
        var (dir, staged) = BootMod(d => Assert.True(d.DefineJson("talents", "boot-talent", json).Ok));
        var node = Assert.Single(LowerBoot(dir, staged)["docs"]!["skills"]!.AsArray())!["document"]!["nodes"]![0]!;
        Loaded(dir);
        var runtime = (string?)Compiler.Lower(NewMod(Boot), "boot-talent", BlockJson.Parse<TalentBlock>(json)).Low.Json["node"]!["iconFile"];
        Assert.Equal(Path.GetFullPath(Path.Combine(dir, "icons/token.png")), runtime);
        Assert.Equal(runtime, (string?)node["iconFile"]);
    }

    [Fact]
    public void A_boot_staged_statuss_png_is_not_in_the_generation_and_its_runtime_activation_sets_it_up_from_the_mods_folder()
    {
        var (dir, staged) = BootMod(d => Assert.True(d.Define("warm", new StatusBlock { Name = "Warm", Seconds = 5, Icon = "icons/token.png" }).Ok));
        var lowered = LowerBoot(dir, staged);
        Assert.DoesNotContain("token.png", lowered["docs"]!.ToJsonString(), StringComparison.Ordinal);   // the buff's clone names no icon: its rule does, at content ready
        BootGeneration.Record(1, [new BootModReport(Boot, 1, BootModState.Committed, ["statuses/warm"], [])],
            new Dictionary<string, IReadOnlyList<BootStaged>> { [Boot] = staged });
        try
        {
            Loaded(dir);
            BootLowering.ActivateNow(Boot);
            var registry = typeof(ModBlocks).Assembly.GetType("Kiln.Blocks", throwOnError: true)!.GetMethod("ModFor", BindingFlags.Static | BindingFlags.NonPublic)!;
            var mod = (ModBlocks)registry.Invoke(null, [Boot, (Func<ulong>)(() => 9)])!;
            Assert.Empty(mod.Errors);
            var icon = Assert.Single(mod.OwnIcons);
            Assert.Equal(("buffs", $"{Boot}.warm"), (icon.Section, icon.Entry));
            Assert.Equal(Path.GetFullPath(Path.Combine(dir, "icons/token.png")), icon.File);
        }
        finally
        {
            BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());
        }
    }
}
