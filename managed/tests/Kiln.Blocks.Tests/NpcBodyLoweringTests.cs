// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class NpcBodyLoweringTests
{
    private static JsonNode Vector() => JsonNode.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Fixtures", "npc-motion-vector.json")))!;

    [Fact]
    public void Managed_bindings_match_the_shared_cook_vector()
    {
        var vector = Vector();
        var mod = vector["mod"]!.GetValue<string>();
        var graph = vector["graph"]!;
        var states = graph["states"]!.AsObject().ToDictionary(x => x.Key, x =>
            (Convert.ToUInt32(x.Value!["id"]!.GetValue<string>()[2..], 16),
             (Guid?)Guid.Parse(x.Value["guid"]!.GetValue<string>()), (string?)x.Value["engine"]));
        var cooked = new NpcCompiler.Graph(graph["name"]!.GetValue<string>(), Guid.Parse((string)graph["guid"]!),
            Guid.Parse((string)graph["skeleton"]!), states, graph["slots"]!.AsObject().ToDictionary(x => x.Key, x => (int)x.Value!), false);
        var rows = NpcBodyRig.Bindings(new BlockContext(new ModBlocks(mod, 1), "npc tobin"),
            BlockJson.Parse<NpcMotionsBlock>(vector["motions"]!.ToJsonString()), mod,
            BlockJson.Parse<AnimationBlock>(vector["animation"]!.ToJsonString()), cooked, (clip, loopDefault) =>
            {
                var key = $"rig.{clip.Name}" + ((clip.Loop ?? loopDefault) ? "" : ".once");
                return (double?)vector["clips"]!["clips"]![key]!["duration"] / (clip.Speed ?? 1);
            });
        Assert.True(JsonNode.DeepEquals(vector["expected"], rows), rows.ToJsonString());
    }

    [Fact]
    public void Authored_lowering_reads_boot_sidecars_and_substitutes_the_Fisher_chassis()
    {
        using var fixture = new CookedBody();
        var file = fixture.Lower();
        Assert.Equal(Bodies.Fisher.Template, (string?)file["template"]);
        Assert.Equal(Bodies.Fisher.Tool, (string?)file["roster"]!["summonItem"]!["base"]);
        Assert.Equal(Bodies.Fisher.Workshop, (string?)file["roster"]!["workshop"]);
        var body = file["body"]!;
        Assert.Equal((string?)fixture.Vector["model"]!["guid"], (string?)body["model"]);
        Assert.Equal((string?)fixture.Vector["graph"]!["guid"], (string?)body["graph"]);
        Assert.Equal((string?)fixture.Vector["model"]!["skeleton"], (string?)body["skeleton"]);
        Assert.True(JsonNode.DeepEquals(fixture.Vector["body"]!["size"], body["size"]));
        Assert.True(JsonNode.DeepEquals(fixture.Vector["expected"], body["bindings"]), body.ToJsonString());
    }

    [Theory]
    [InlineData("skeleton", "body.model")]
    [InlineData("bounds", "body.size")]
    [InlineData("slots", "body.slots[Head]")]
    [InlineData("state-guid", "motions.bindings[11].state")]
    [InlineData("model", "body.model")]
    [InlineData("graph", "animation")]
    [InlineData("clips", "motions.optionalFallback")]
    [InlineData("source", "body.model")]
    public void Authored_lowering_checks_real_sidecars_and_names_the_bad_field(string change, string field)
    {
        using var fixture = new CookedBody();
        switch (change)
        {
            case "skeleton": fixture.Vector["model"]!["skeleton"] = Guid.NewGuid().ToString(); break;
            case "bounds": fixture.Vector["model"]!["boundingBox"]!["min"]!["y"] = -0.1; break;
            case "slots": fixture.Vector["graph"]!["slots"]!.AsObject().Remove("Head"); break;
            case "state-guid": fixture.Vector["graph"]!["states"]!["motion-interaction-0"]!["guid"] = null; break;
        }
        fixture.Write();
        if (change is "model" or "graph" or "clips" or "source") File.Delete(fixture.PathOf(change));
        Assert.Equal(field, Assert.Throws<BlockError>(() => fixture.Lower()).Field);
    }

    private sealed class CookedBody : IDisposable
    {
        internal readonly JsonNode Vector = NpcBodyLoweringTests.Vector();
        private readonly string root = Path.Combine(Path.GetTempPath(), "kiln-npc-body-" + Guid.NewGuid().ToString("N"));
        private readonly Func<BlockCapabilities.Row>? previous = BlockCapabilities.NpcBodyForTests;
        internal CookedBody()
        {
            Directory.CreateDirectory(Path.Combine(root, "assets", "cooked"));
            Directory.CreateDirectory(Path.Combine(root, "models"));
            File.WriteAllText(PathOf("source"), "{}");
            Write();
            BlockCapabilities.NpcBodyForTests = () => new(CapabilityState.Available, CapabilityReason.None, null);
        }
        internal string PathOf(string kind) => kind == "source" ? Path.Combine(root, "models", "rig.gltf") :
            Path.Combine(root, "assets", "cooked", kind switch
            {
                "graph" => "tobin.animation.tobin.body.graph.json", "model" => "models_rig.model.json", _ => "clips.json",
            });
        internal void Write()
        {
            foreach (var kind in new[] { "graph", "model", "clips" }) File.WriteAllText(PathOf(kind), Vector[kind]!.ToJsonString());
        }
        internal JsonNode Lower()
        {
            var mod = new ModBlocks(Vector["mod"]!.GetValue<string>(), 1) { ManifestClients = "mod", BootDirectory = root };
            var (_, low, _) = Compiler.Lower(mod, "tobin", new NpcBlock
            {
                Name = "Tobin", Body = BlockJson.Parse<BodyBlock>(Vector["body"]!.ToJsonString()),
                Animation = BlockJson.Parse<AnimationBlock>(Vector["animation"]!.ToJsonString()),
                Motions = BlockJson.Parse<NpcMotionsBlock>(Vector["motions"]!.ToJsonString()), Roster = new RosterBlock(),
            });
            return low.Json["npc"]!;
        }
        public void Dispose()
        {
            BlockCapabilities.NpcBodyForTests = previous;
            Directory.Delete(root, true);
        }
    }
}
