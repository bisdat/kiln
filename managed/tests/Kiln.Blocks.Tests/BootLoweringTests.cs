// SPDX-License-Identifier: MIT
// Mod.OnBoot's lowering (chapter 08 §4.3 steps 1-3, S5): staged definitions become the collectors' documents through the same Compiler.Lower
// a runtime Define uses, with a sink that registers nothing; the same input gives the same ids; a data file of the mod wins.
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Blocks.Tests;

[Collection("boot generation")]  // BootGeneration is process-wide
public sealed class BootLoweringTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "kiln-boot-lowering-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, true);
    }

    private static List<BootStaged> Sample()
    {
        var f = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        var d = BootDefinitions.For(f);
        Assert.True(d.Define("cold", new StatusBlock { Name = "Boot Cold", Seconds = 4 }).Ok);
        Assert.True(d.Define("cold-hit", new EffectBlock { Applies = new StatusApply { Status = "cold" } }).Ok);
        Assert.True(d.Define("wood-token", new ItemBlock { Name = "Boot Token", Stack = 20 }).Ok);
        Assert.True(d.Define("cold-talent", new TalentBlock { Name = "Boot Cold Talent", Keeps = "cold", Game = new GameTreePlace { From = ["0x8efcc472"], Offset = [170, 0] } }).Ok);
        return f.Staged;
    }

    private string Request(IEnumerable<BootStaged> staged, string? dir = null) => new JsonObject
    {
        ["id"] = "boot-gallery", ["clients"] = "mod", ["dir"] = dir,
        ["staged"] = new JsonArray(staged.Select(s => (JsonNode)new JsonObject { ["kind"] = s.Kind, ["key"] = s.Key, ["json"] = s.Json }).ToArray()),
    }.ToJsonString();

    private static JsonObject Lower(string request) => JsonNode.Parse(BootLowering.LowerOwner(request))!.AsObject();

    [Fact]
    public void A_committed_mod_that_staged_nothing_still_gets_its_event()
    {
        BootGeneration.Record(1, [new BootModReport("files-only", 1, BootModState.Committed, [], [])], new Dictionary<string, IReadOnlyList<BootStaged>>());
        try
        {
            var got = new List<BootDefinitionsCommittedEvent>();
            using var sub = Events.On<BootDefinitionsCommittedEvent>(got.Add);
            BootLowering.ActivateNow("files-only");
            var e = Assert.Single(got);
            Assert.Equal(("files-only", 1ul), (e.ModId, e.Generation));
            Assert.Empty(e.Definitions);
        }
        finally
        {
            BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());
        }
    }

    [Fact]
    public void A_resource_patch_is_staged_as_its_document_and_goes_to_the_patches_collector()
    {
        var f = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        var d = BootDefinitions.For(f);
        var table = Guid.Parse("82706b40-61b1-4b8f-8b23-dcec6971bda1");
        var staged = d.Define("more-altars", new ResourcePatchBlock
        {
            Resource = table, Type = "keen::BalancingTable",
            Set =
            {
                ["altarsPerFlameLevel[*]"] = ResourceValue.AtLeast(16), ["experienceNeedPerLevel[0]"] = 1500, ["someFlag"] = true,
                ["someEnum"] = "Hard", ["scale"] = new ResourceValue { Mul = 2, Add = 1, Max = 9.5, RaiseOnly = true },
            },
        });
        Assert.True(staged.Ok, staged.Reason);
        var one = Assert.Single(f.Staged);
        Assert.Equal("resourcePatches", one.Kind);
        // the document kiln.content reads, keys sorted, member paths as written
        Assert.Equal("""{"part":0,"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","set":{"altarsPerFlameLevel[*]":{"min":16},"experienceNeedPerLevel[0]":1500,"scale":{"add":1,"max":9.5,"mul":2,"raiseOnly":true},"someEnum":"Hard","someFlag":true},"type":"keen::BalancingTable"}""", one.Json);
        // the same from JSON
        var g = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        Assert.True(BootDefinitions.For(g).DefineJson("resourcePatches", "more-altars", one.Json).Ok);
        Assert.Equal(one.Json, Assert.Single(g.Staged).Json);
        // lowered: one document for the patches collector, nothing compiled, nothing registered
        var sink = new CountingSink();
        var previous = Lowered._sinkOverride;
        Lowered._sinkOverride = sink;
        try
        {
            var r = Lower(Request(f.Staged));
            Assert.True(r["ok"]!.GetValue<bool>(), (string?)r["reason"]);
            Assert.Equal(["resourcePatches/more-altars"], r["definitions"]!.AsArray().Select(x => (string)x!));
            var doc = Assert.Single(r["docs"]!["patches"]!.AsArray())!;
            Assert.Equal("more-altars", (string?)doc["key"]);
            Assert.Equal("keen::BalancingTable", (string?)doc["document"]!["type"]);
            Assert.Equal(16, (double)doc["document"]!["set"]!["altarsPerFlameLevel[*]"]!["min"]!);
            Assert.Empty(r["docs"]!["content"]!.AsArray());
            Assert.Equal(0, sink.Count);
        }
        finally { Lowered._sinkOverride = previous; }
    }

    [Fact]
    public void A_resource_patch_that_names_nothing_or_mixes_a_value_with_a_change_is_refused()
    {
        var f = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        var d = BootDefinitions.For(f);
        Assert.ThrowsAny<Exception>(() => d.Define("both", new ResourcePatchBlock
        {
            Resource = Guid.NewGuid(), Type = "keen::BalancingTable", Set = { ["a"] = new ResourceValue { Value = 1L, Min = 2 } },
        }));
        Assert.Throws<ArgumentException>(() => d.DefineJson("resourcePatches", "bad-op", """{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"keen::BalancingTable","set":{"a":{"double":2}}}"""));
        // staged but empty: the lowering refuses the author
        Assert.True(d.Define("empty", new ResourcePatchBlock { Resource = Guid.NewGuid(), Type = "keen::BalancingTable" }).Ok);
        var r = Lower(Request(f.Staged));
        Assert.False(r["ok"]!.GetValue<bool>());
        Assert.Contains("at least one change", (string?)r["reason"]);
    }

    [Fact]
    public void A_resource_patch_can_change_a_templates_components_and_append_to_a_list()
    {
        var f = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        var d = BootDefinitions.For(f);
        var staged = d.Define("ground-pickup", new ResourcePatchBlock
        {
            Resource = Guid.Parse("82706b40-61b1-4b8f-8b23-dcec6971bda1"), Type = "keen::ecs::TemplateResource",
            RemoveComponents = ["SpawnTime"],
            AddComponents = ["PickupItemZone"],
            Set = { ["PickupItemZone.lastAttempt.value"] = 0L },
        });
        Assert.True(staged.Ok, staged.Reason);
        Assert.Equal("""{"addComponents":["PickupItemZone"],"part":0,"removeComponents":["SpawnTime"],"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","set":{"PickupItemZone.lastAttempt.value":0},"type":"keen::ecs::TemplateResource"}""",
            Assert.Single(f.Staged).Json);
        // an append alone is a change; values above the list's last only
        Assert.True(d.Define("steps", new ResourcePatchBlock
        {
            Resource = Guid.NewGuid(), Type = "keen::FbUiBundle",
            Append = new() { ["difficultySettings.settingValues.enemyDamage.steps"] = new ResourceListAppend { Values = [2.5, 3, 100], AboveLast = true } },
        }).Ok);
        var json = f.Staged[1].Json;
        Assert.Contains(""""append":{"difficultySettings.settingValues.enemyDamage.steps":{"aboveLast":true,"values":[2.5,3,100]}}"""", json);
        var r = Lower(Request(f.Staged));
        Assert.True(r["ok"]!.GetValue<bool>(), (string?)r["reason"]);
        Assert.Equal(2, r["docs"]!["patches"]!.AsArray().Count);
    }

    [Fact]
    public void A_resource_patch_can_copy_a_component_and_clear_or_replace_lists()
    {
        var f = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        var d = BootDefinitions.For(f);
        var magic = Guid.Parse("955ba32f-08ea-414d-bd98-ac75990aef04");
        var staged = d.Define("stock", new ResourcePatchBlock
        {
            Resource = Guid.Parse("65baa890-022a-454b-9cb7-b92cb8c06cd4"), Type = "keen::ecs::TemplateResource",
            CopyComponents = new() { ["InventoryCraftingStock"] = magic },
            Set = { ["InventoryCraftingStock.canReceiveAreaDeposit"] = true },
        });
        Assert.True(staged.Ok, staged.Reason);
        Assert.Equal("""{"copyComponents":{"InventoryCraftingStock":"955ba32f-08ea-414d-bd98-ac75990aef04"},"part":0,"resource":"65baa890-022a-454b-9cb7-b92cb8c06cd4","set":{"InventoryCraftingStock.canReceiveAreaDeposit":true},"type":"keen::ecs::TemplateResource"}""",
            Assert.Single(f.Staged).Json);
        // a clear alone and a replace alone are changes
        Assert.True(d.Define("anywhere", new ResourcePatchBlock
        {
            Resource = Guid.NewGuid(), Type = "keen::SceneResource", Clear = ["noBuildZones", "resetPlayersOutOfBounds.playableAreas"],
        }).Ok);
        Assert.Contains(""""clear":["noBuildZones","resetPlayersOutOfBounds.playableAreas"]"""", f.Staged[1].Json);
        Assert.True(d.Define("steps", new ResourcePatchBlock
        {
            Resource = Guid.NewGuid(), Type = "keen::FbUiBundle",
            Replace = new() { ["difficultySettings.settingValues.enemyDamage.steps"] = new ResourceListReplace { Values = [0.5, 1, 2] } },
        }).Ok);
        Assert.Contains(""""replace":{"difficultySettings.settingValues.enemyDamage.steps":{"values":[0.5,1,2]}}"""", f.Staged[2].Json);
        var r = Lower(Request(f.Staged));
        Assert.True(r["ok"]!.GetValue<bool>(), (string?)r["reason"]);
        Assert.Equal(3, r["docs"]!["patches"]!.AsArray().Count);
        // the JSON form reads back into the record
        var back = JsonSerializer.Deserialize<ResourcePatchBlock>(f.Staged[0].Json, BlockJson.SharedOptions)!;
        Assert.Equal(magic, back.CopyComponents!["InventoryCraftingStock"]);
    }

    [Fact]
    public void A_replace_takes_plain_values_and_a_copy_names_another_template()
    {
        var f = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        var d = BootDefinitions.For(f);
        var self = Guid.NewGuid();
        Assert.Contains("not a change of the game's value", Assert.Throws<ArgumentException>(() => d.Define("doubled", new ResourcePatchBlock
        {
            Resource = self, Type = "keen::FbUiBundle", Replace = new() { ["steps"] = new ResourceListReplace { Values = [ResourceValue.Times(2)] } },
        })).Message);
        Assert.Contains("Clear empties a list", Assert.Throws<ArgumentException>(() => d.Define("none", new ResourcePatchBlock
        {
            Resource = self, Type = "keen::FbUiBundle", Replace = new() { ["steps"] = new ResourceListReplace() },
        })).Message);
        Assert.Contains("not the one patched", Assert.Throws<ArgumentException>(() => d.Define("itself", new ResourcePatchBlock
        {
            Resource = self, Type = "keen::ecs::TemplateResource", CopyComponents = new() { ["Gravity"] = self },
        })).Message);
        Assert.Throws<ArgumentException>(() => d.Define("empty-path", new ResourcePatchBlock { Resource = self, Type = "keen::SceneResource", Clear = [""] }));
        Assert.Throws<ArgumentException>(() => d.DefineJson("resourcePatches", "bad-copy",
            """{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"keen::ecs::TemplateResource","copyComponents":{"Gravity":"not-a-guid"}}"""));
        Assert.Throws<ArgumentException>(() => d.DefineJson("resourcePatches", "bad-replace",
            """{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"keen::FbUiBundle","replace":{"steps":{"values":[1],"aboveLast":true}}}"""));
        Assert.Empty(f.Staged);
    }

    [Fact]
    public void An_appended_value_is_a_plain_value_not_a_change_of_the_games_value()
    {
        var f = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        var d = BootDefinitions.For(f);
        var e = Assert.Throws<ArgumentException>(() => d.Define("doubled", new ResourcePatchBlock
        {
            Resource = Guid.NewGuid(), Type = "keen::FbUiBundle",
            Append = new() { ["steps"] = new ResourceListAppend { Values = [3, ResourceValue.Times(2)] } },
        }));
        Assert.Contains("not a change of the game's value", e.Message);
        Assert.Throws<ArgumentException>(() => d.Define("none", new ResourcePatchBlock
        {
            Resource = Guid.NewGuid(), Type = "keen::FbUiBundle", Append = new() { ["steps"] = new ResourceListAppend() },
        }));
        e = Assert.Throws<ArgumentException>(() => d.DefineJson("resourcePatches", "doubled-json",
            """{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"keen::FbUiBundle","append":{"steps":{"values":[{"mul":2}]}}}"""));
        Assert.Contains("not a change of the game's value", e.Message);
        Assert.Empty(f.Staged);
    }

    [Fact]
    public void The_sample_lowers_into_documents_of_assets_content_and_skills_and_registers_nothing()
    {
        var sink = new CountingSink();
        var previous = Lowered._sinkOverride;
        Lowered._sinkOverride = sink;  // anything that escapes the boot sink lands here
        try
        {
            var r = Lower(Request(Sample()));
            Assert.True(r["ok"]!.GetValue<bool>(), (string?)r["reason"]);
            Assert.Equal(["statuses/cold", "effects/cold-hit", "items/wood-token", "talents/cold-talent"], r["definitions"]!.AsArray().Select(x => (string)x!));
            var docs = r["docs"]!.AsObject();
            var content = docs["content"]!.AsArray();
            Assert.Contains(content, d => (string?)d!["document"]!["op"] == "clone" && (string?)d["document"]!["section"] == "buffs");
            Assert.Contains(content, d => (string?)d!["document"]!["op"] == "define" && (string?)d["document"]!["kind"] == "items");
            var program = docs["assets"]!.AsArray().First()!["document"]!["entries"]![0]!;
            Assert.Equal("0xd4922d88", (string?)program["type"]);
            Assert.NotEmpty(Convert.FromBase64String((string)program["data"]!));
            var node = Assert.Single(docs["skills"]!.AsArray())!["document"]!["nodes"]![0]!;
            Assert.Equal("0x8efcc472", (string?)node["from"]![0]);
            Assert.Equal(0, sink.Count);
            Assert.False(Lowered._bootLowering);
        }
        finally
        {
            Lowered._sinkOverride = previous;
        }
    }

    [Fact]
    public void A_runtime_Define_and_the_boot_lowering_give_the_same_ids()
    {
        var r = Lower(Request(Sample()));
        var buff = r["docs"]!["content"]!.AsArray().First(d => (string?)d!["document"]!["section"] == "buffs")!["document"]!["definition"]!;
        var item = r["docs"]!["content"]!.AsArray().First(d => (string?)d!["document"]!["kind"] == "items")!["document"]!["definition"]!;
        var mod = new ModBlocks("boot-gallery", 1) { ManifestClients = "mod" };
        var (status, _, _) = Compiler.Lower(mod, "cold", new StatusBlock { Name = "Boot Cold", Seconds = 4 });
        var (token, _, _) = Compiler.Lower(mod, "wood-token", new ItemBlock { Name = "Boot Token", Stack = 20 });
        Assert.Equal(status.Guid.ToString(), (string?)buff["guid"]);
        Assert.Equal(token.Guid.ToString(), (string?)item["guid"]);
        // and the same input lowers to the same bytes again
        Assert.Equal(r.ToJsonString(), Lower(Request(Sample())).ToJsonString());
    }

    [Fact]
    public void A_data_file_of_the_mod_wins_over_staged_code()
    {
        Directory.CreateDirectory(Path.Combine(_dir, "blocks"));
        File.WriteAllText(Path.Combine(_dir, "blocks", "gallery.json"), """{ "statuses": { "cold": { "name": "File Cold", "seconds": 9 } } }""");
        var r = Lower(Request(Sample().Where(s => s.Kind == "statuses" || s.Kind == "items"), _dir));
        Assert.True(r["ok"]!.GetValue<bool>());
        Assert.Equal(["items/wood-token"], r["definitions"]!.AsArray().Select(x => (string)x!));
        var issue = Assert.Single(r["issues"]!.AsArray())!;
        Assert.Equal("ShadowedByFile", (string?)issue["code"]);
        Assert.Equal("cold", (string?)issue["key"]);
        Assert.DoesNotContain(r["docs"]!["content"]!.AsArray(), d => (string?)d!["document"]!["section"] == "buffs");
    }

    [Fact]
    public void A_boot_definition_that_names_another_mods_block_or_a_data_file_block_is_refused_by_name()
    {
        var f = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        Assert.True(BootDefinitions.For(f).Define("frost-hit", new EffectBlock { Applies = new StatusApply { Status = "other-mod:frost" } }).Ok);
        var r = Lower(Request(f.Staged));
        Assert.False(r["ok"]!.GetValue<bool>());
        Assert.Contains("BootReferenceOutside", (string?)r["reason"]);
        Assert.Contains("mod other-mod", (string?)r["reason"]);

        // the mod's own data file: "chill" is there, so the staged effect cannot name it
        Directory.CreateDirectory(Path.Combine(_dir, "blocks"));
        File.WriteAllText(Path.Combine(_dir, "blocks", "gallery.json"), """{ "statuses": { "chill": { "name": "File Chill", "seconds": 9 } } }""");
        var g = new BootContext("boot-gallery", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        Assert.True(BootDefinitions.For(g).Define("chill-hit", new EffectBlock { Applies = new StatusApply { Status = "chill" } }).Ok);
        r = Lower(Request(g.Staged, _dir));
        Assert.False(r["ok"]!.GetValue<bool>());
        Assert.Contains("BootReferenceOutside", (string?)r["reason"]);
        Assert.Contains("data files", (string?)r["reason"]);

        // staged before it in the same OnBoot: fine (the sample's cold-hit names cold)
        Assert.True(Lower(Request(Sample()))["ok"]!.GetValue<bool>());
    }

    [Fact]
    public void A_definition_that_does_not_compile_refuses_the_author()
    {
        var staged = new List<BootStaged> { new("statuses", "nameless", """{"seconds":4}""") };
        var r = Lower(Request(staged));
        Assert.False(r["ok"]!.GetValue<bool>());
        Assert.Contains("statuses/nameless", (string?)r["reason"]);
    }

    private sealed class CountingSink : Lowered.ILoweredSink
    {
        public int Count;
        public void Program(ulong owner, Guid guid, byte[] blob) => Count++;
        public void Clone(ulong owner, string section, JsonObject def) => Count++;
        public void Item(ulong owner, ItemDefinition d) => Count++;
        public void Recipe(ulong owner, RecipeDefinition d) => Count++;
    }
}
