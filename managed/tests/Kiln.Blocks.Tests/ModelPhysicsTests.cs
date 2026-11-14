// SPDX-License-Identifier: MIT
// A body's cloth and chains (Npc/NpcPhysics.cs, chapter 16e): the typed and the JSON form are one request, byte for byte the cook's
// (tools/modelcook/tests/test_physics_input.py SpecHash holds the same hashes), their checks name the field, a body asks for what its build
// cooked, and a mod hears once that its cloth waits for a game test.
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class ModelPhysicsTests : IDisposable
{
    // The cook's spec hash of the same request (tools/modelcook/cook/physics.py spec_hash): keep both tests' values equal.
    internal const string FullHash = "e5c0cae78d515c24b5e5ec5301716a18b3de6f0278709821363d6c6c2b11b5f1";
    internal const string MinimalHash = "48447f8115000ff8fb08bf190e914f9ebd9e8367f9ddea662c3aa179cea5b2f4";

    private const string Json = """
        { "model": "models/cape.gltf",
          "cloth": [ { "mesh": 0, "primitive": 1, "particleIds": [7, 3, 3, 9], "pin": [1, 0, 0.25], "massKg": 0.3,
                       "colliders": [ { "type": "sphere", "x": 0, "y": 1.5, "z": -0.1, "radius": 0.15, "joint": "spine" },
                                      { "type": "capsule", "a": { "x": 0.1, "y": 1, "z": 0, "radius": 0.1, "joint": "j2" },
                                                           "b": { "x": 0.1, "y": 0.4, "z": 0, "radius": 0.08, "joint": "j2" } } ] } ],
          "chains": [ { "rootJoint": "zöpf_0", "tipJoint": "zöpf_2", "particleRadius": 0.02 } ] }
        """;

    private static BodyBlock Typed() => new()
    {
        Model = "models/cape.gltf",
        Cloth =
        [
            new ModelClothBlock
            {
                Mesh = 0, Primitive = 1, ParticleIds = [7, 3, 3, 9], Pin = [1, 0, 0.25], MassKg = 0.3,
                Colliders = [new ClothSphereBlock(0, 1.5, -0.1, 0.15, "spine"), new ClothCapsuleBlock(new(0.1, 1, 0, 0.1, "j2"), new(0.1, 0.4, 0, 0.08, "j2"))],
            },
        ],
        Chains = [new ModelChainBlock { RootJoint = "zöpf_0", TipJoint = "zöpf_2", ParticleRadius = 0.02 }],
    };

    private static ModBlocks Mod(string id) => new(id, 1);

    private readonly Func<string, bool> _held = NpcPhysics.IsHeld;

    public void Dispose() => NpcPhysics.IsHeld = _held;

    [Fact]
    public void The_typed_and_the_json_form_are_one_request_and_the_cooks()
    {
        var typed = NpcPhysics.SpecBytes(Typed());
        var json = NpcPhysics.SpecBytes(BlockJson.Parse<BodyBlock>(Json));
        Assert.NotNull(typed);
        Assert.Equal(typed, json);
        Assert.Equal(FullHash, NpcPhysics.SpecHash(Typed()));
        Assert.Equal(MinimalHash, NpcPhysics.SpecHash(new BodyBlock { Model = "m.gltf", Cloth = [new ModelClothBlock { Pin = [1] }] }));
        Assert.Null(NpcPhysics.SpecHash(new BodyBlock { Model = "m.gltf" }));
        Assert.Null(NpcPhysics.SpecHash(new BodyBlock { Model = "m.gltf", Cloth = [], Chains = [] }));
        // any change is another request
        Assert.NotEqual(FullHash, NpcPhysics.SpecHash(Typed() with { Chains = [new ModelChainBlock { RootJoint = "zöpf_0", TipJoint = "zöpf_2", ParticleRadius = 0.03 }] }));
    }

    [Fact]
    public void The_json_form_round_trips_and_names_each_collider_by_type()
    {
        var text = JsonSerializer.Serialize(Typed(), BlockJson.SharedOptions);
        var node = JsonNode.Parse(text)!;
        Assert.Equal("sphere", (string?)node["cloth"]![0]!["colliders"]![0]!["type"]);
        Assert.Equal("capsule", (string?)node["cloth"]![0]!["colliders"]![1]!["type"]);
        Assert.Null(node["cloth"]![0]!["colliders"]![1]!["a"]!["type"]);
        Assert.Equal(NpcPhysics.SpecBytes(Typed()), NpcPhysics.SpecBytes(BlockJson.Parse<BodyBlock>(text)));
        // an end of a capsule has no "type" (the cook refuses it too), and a collider needs one
        Assert.Throws<JsonException>(() => BlockJson.Parse<BodyBlock>(Json.Replace("\"a\": { \"x\"", "\"a\": { \"type\": \"sphere\", \"x\"")));
        // (System.Text.Json says so with NotSupportedException; the block loader reports it as the block's error, as for every "type")
        Assert.Throws<NotSupportedException>(() => BlockJson.Parse<BodyBlock>(Json.Replace("{ \"type\": \"sphere\", ", "{ ")));
        Assert.Throws<JsonException>(() => BlockJson.Parse<BodyBlock>(Json.Replace("\"massKg\"", "\"mass\"")));
    }

    private static BlockError Refused(BodyBlock body) => Assert.Throws<BlockError>(() => Compiler.Define(Mod("phys-refused"), "cape", body));

    public static TheoryData<string, string, string> Refusals() => new()
    {
        { "cloth[0].pin[1]", "1.5 is outside [0, 1]", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1, 1.5] } ] }""" },
        { "cloth[0].pin", "one value", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0 } ] }""" },
        { "cloth[0].pin", "2 values for 3 particles", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "particleIds": [0, 1, 2], "pin": [1, 0] } ] }""" },
        { "cloth[0].particleIds[1]", "-1 is outside", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "particleIds": [0, -1], "pin": [1, 0] } ] }""" },
        { "cloth[0].mesh", "-2 is outside", """{ "model": "m.gltf", "cloth": [ { "mesh": -2, "primitive": 0, "pin": [1] } ] }""" },
        { "cloth[0].massKg", "0 is outside [0.001, 100]", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1], "massKg": 0 } ] }""" },
        { "cloth[0].gravity", "51 is outside [0, 50]", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1], "gravity": 51 } ] }""" },
        { "cloth[0].damping", "1 is outside [0, 0.99]", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1], "damping": 1 } ] }""" },
        { "cloth[0].stiffness", "0 is outside (0, 1]", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1], "stiffness": 0 } ] }""" },
        { "cloth[1]", "already cloth in cloth[0]", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1] }, { "mesh": 0, "primitive": 0, "pin": [1] } ] }""" },
        { "cloth[0].colliders[0].radius", "9 is outside (0, 8]", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1], "colliders": [ { "type": "sphere", "x": 0, "y": 0, "z": 0, "radius": 9 } ] } ] }""" },
        { "cloth[0].colliders[0].b.joint", "both ends of a capsule follow one joint", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1], "colliders": [ { "type": "capsule", "a": { "x": 0, "y": 0, "z": 0, "radius": 1, "joint": "a" }, "b": { "x": 0, "y": 1, "z": 0, "radius": 1, "joint": "b" } } ] } ] }""" },
        { "cloth[0].colliders[0]", "at one point", """{ "model": "m.gltf", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1], "colliders": [ { "type": "capsule", "a": { "x": 0, "y": 0, "z": 0, "radius": 1 }, "b": { "x": 0, "y": 0, "z": 0, "radius": 1 } } ] } ] }""" },
        { "chains[0].tipJoint", "is the root too", """{ "model": "m.gltf", "chains": [ { "rootJoint": "a", "tipJoint": "a" } ] }""" },
        { "chains[0].rootJoint", "a joint's name", """{ "model": "m.gltf", "chains": [ { "tipJoint": "a" } ] }""" },
        { "chains[0].stiffness", "0.5 is outside [1, 1000000]", """{ "model": "m.gltf", "chains": [ { "rootJoint": "a", "tipJoint": "b", "stiffness": 0.5 } ] }""" },
        { "chains[0].particleRadius", "2 is outside [0, 1]", """{ "model": "m.gltf", "chains": [ { "rootJoint": "a", "tipJoint": "b", "particleRadius": 2 } ] }""" },
        { "chains[1]", "already declared in chains[0]", """{ "model": "m.gltf", "chains": [ { "rootJoint": "a", "tipJoint": "b" }, { "rootJoint": "a", "tipJoint": "b", "damping": 0.5 } ] }""" },
        { "scale", "keeps scale 1", """{ "model": "m.gltf", "scale": 2, "chains": [ { "rootJoint": "a", "tipJoint": "b" } ] }""" },
        { "cloth", "a game body has its own", """{ "like": "vukah", "cloth": [ { "mesh": 0, "primitive": 0, "pin": [1] } ] }""" },
    };

    [Theory]
    [MemberData(nameof(Refusals))]
    public void Each_refusal_names_its_field(string field, string text, string json)
    {
        var e = Refused(BlockJson.Parse<BodyBlock>(json));
        Assert.Equal(field, e.Field);
        Assert.Contains(text, e.Problem, StringComparison.Ordinal);
    }

    [Fact]
    public void The_models_colliders_count_each_once_up_to_32()
    {
        ClothColliderBlock S(int i) => new ClothSphereBlock(i, 0, 0, 0.1);
        var same = Enumerable.Range(0, 20).Select(S).ToList();
        Compiler.Define(Mod("phys-colliders"), "cape", new BodyBlock
        {
            Model = "m.gltf",
            Cloth = [new ModelClothBlock { Pin = [1], Colliders = same }, new ModelClothBlock { Mesh = 1, Pin = [1], Colliders = same }],
        });
        var e = Refused(new BodyBlock
        {
            Model = "m.gltf",
            Cloth = [new ModelClothBlock { Pin = [1], Colliders = same }, new ModelClothBlock { Mesh = 1, Pin = [1], Colliders = Enumerable.Range(20, 13).Select(S).ToList() }],
        });
        Assert.Equal("cloth", e.Field);
        Assert.Contains("come to 33 spheres", e.Problem, StringComparison.Ordinal);
        var capsules = Enumerable.Range(0, 17).Select(i => (ClothColliderBlock)new ClothCapsuleBlock(new(i, 0, 0, 0.1), new(i, 1, 0, 0.1))).ToList();
        Assert.Equal("cloth[0].colliders", Refused(new BodyBlock { Model = "m.gltf", Cloth = [new ModelClothBlock { Pin = [1], Colliders = capsules }] }).Field);
    }

    [Fact]
    public void A_body_asks_for_what_its_build_cooked()
    {
        var ctx = new BlockContext(Mod("phys-cooked"), "creature golem");
        var body = Typed();
        NpcPhysics.CheckCooked(ctx, body, new JsonObject { ["physics"] = new JsonObject { ["spec"] = FullHash, ["cloth"] = 1, ["chains"] = 1 } }, "models/cape", "body");
        var missing = Assert.Throws<BlockError>(() => NpcPhysics.CheckCooked(ctx, body, new JsonObject { ["guid"] = Guid.Empty.ToString() }, "models/cape", "body"));
        Assert.Equal("body.cloth", missing.Field);
        Assert.Contains("cooked without cloth or chains", missing.Problem, StringComparison.Ordinal);
        Assert.Contains("never while the game runs", missing.Problem, StringComparison.Ordinal);
        var other = Assert.Throws<BlockError>(() => NpcPhysics.CheckCooked(ctx, body, new JsonObject { ["physics"] = new JsonObject { ["spec"] = MinimalHash } }, "models/cape", "body"));
        Assert.Contains("cooked with other cloth or chains", other.Problem, StringComparison.Ordinal);
        // a body without cloth asks nothing: its model may have cloth from assets/physics.json
        NpcPhysics.CheckCooked(ctx, new BodyBlock { Model = "models/cape.gltf" }, new JsonObject { ["physics"] = new JsonObject { ["spec"] = FullHash } }, "models/cape", "body");
        var chainsOnly = Assert.Throws<BlockError>(() => NpcPhysics.CheckCooked(ctx, new BodyBlock { Model = "m.gltf", Chains = [new ModelChainBlock { RootJoint = "a", TipJoint = "b" }] }, new JsonObject(), "m", "body"));
        Assert.Equal("body.chains", chainsOnly.Field);
    }

    [Fact]
    public void A_held_row_is_said_once_per_mod()
    {
        NpcPhysics.IsHeld = name => name is NpcPhysics.ClothRow or NpcPhysics.ChainsRow;
        var mod = Mod("phys-held");
        Compiler.Define(mod, "cape", Typed() with { Chains = null });
        Compiler.Define(mod, "cloak", Typed() with { Chains = null, Model = "models/cloak.gltf" });
        Assert.Single(mod.Warnings, w => w.StartsWith("modelcook.cloth:", StringComparison.Ordinal));
        Assert.DoesNotContain(mod.Warnings, w => w.StartsWith("modelcook.chains2:", StringComparison.Ordinal));
        Compiler.Define(mod, "pendant", new BodyBlock { Model = "models/pendant.gltf", Chains = [new ModelChainBlock { RootJoint = "a", TipJoint = "b" }] });
        Assert.Single(mod.Warnings, w => w.StartsWith("modelcook.chains2:", StringComparison.Ordinal));
        Assert.Equal(2, mod.Warnings.Count);
        Assert.Contains("\"preview\" in kiln.json", mod.Warnings[0], StringComparison.Ordinal);
        // another mod hears it too
        var other = Mod("phys-held-2");
        Compiler.Define(other, "cape", Typed() with { Chains = null });
        Assert.Single(other.Warnings);
    }

    [Fact]
    public void Nothing_is_said_when_the_row_is_open_or_the_body_has_no_physics()
    {
        NpcPhysics.IsHeld = _ => false;
        var mod = Mod("phys-open");
        Compiler.Define(mod, "cape", Typed());
        Assert.Empty(mod.Warnings);
        NpcPhysics.IsHeld = _ => true;
        var plain = Mod("phys-plain");
        Compiler.Define(plain, "golem", new BodyBlock { Model = "models/golem.gltf" });
        Assert.Empty(plain.Warnings);
    }
}
