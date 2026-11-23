// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class NpcBodyTests
{
    private static readonly BlockContext Context = new(new ModBlocks("npc-body-test", 1), "npc tobin");
    private static readonly Guid Skeleton = new("c93501f7-9054-54ed-83f4-78309584cb37");
    private static BodyBlock Body() => new()
    {
        Model = "models/tobin.glb", Size = new BodySize(0.75, 2),
        Slots = new Dictionary<Slot, string> { [Slot.Root] = "root", [Slot.Head] = "head", [Slot.HandR] = "hand_r" },
    };

    private static NpcMotionsBlock Motions() => new()
    {
        Bindings = Enum.GetValues<NpcMotionRole>().SelectMany(role => Enumerable.Range(0, NpcBodyRig.Variants(role)).Select(v =>
            new NpcMotionBinding(role, v, new AnimState($"{role}-{v}"), role == NpcMotionRole.Interaction ? NpcBodyRig.FisherInteraction : null))).ToArray(),
    };

    private static AnimationBlock Animation() => new()
    {
        Clips = "models/tobin.glb", Idle = "Idle",
        States = Motions().Bindings.ToDictionary(b => b.State, b => new Clip("Pose") { Loop = b.Role is NpcMotionRole.Sit or NpcMotionRole.Sleep && b.Variant == 1 }),
    };

    private static NpcCompiler.Graph Graph() => new("moves.body", Guid.NewGuid(), Skeleton,
        Motions().Bindings.Select(b => b.State).Append(AnimState.Idle).Append(AnimState.Run).ToDictionary(s => s.Name,
            s => (s.Hash("npc-body-test"), s.Guid("npc-body-test"), s.EngineName)), new() { ["Root"] = 0, ["Head"] = 2, ["HandR"] = 3 }, false);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Interaction_uses_the_chassis_key_when_omitted_or_null_and_still_accepts_the_exact_key(bool explicitKey)
    {
        var motions = Motions() with { Bindings = Motions().Bindings.Select(b => b.Role == NpcMotionRole.Interaction
            ? b with { InteractionKey = explicitKey ? NpcBodyRig.FisherInteraction : null } : b).ToArray() };
        var parsed = BlockJson.Parse<NpcMotionsBlock>(JsonSerializer.Serialize(motions, BlockJson.SharedOptions));
        var rows = NpcBodyRig.Bindings(Context, parsed, "npc-body-test", Animation(), Graph(), (_, _) => 1);
        var interaction = rows.Single(row => (string?)row!["role"] == "interaction")!;
        Assert.Equal(NpcBodyRig.FisherInteraction.ToString(), (string?)interaction["interactionKey"]);
    }

    [Theory]
    [InlineData("00000000-0000-0000-0000-000000000000")]
    [InlineData("12345678-1234-5678-9234-123456789abc")]
    public void A_nondefault_interaction_key_must_still_match_the_chassis(string key)
    {
        var motions = Motions() with { Bindings = Motions().Bindings.Select(b => b.Role == NpcMotionRole.Interaction
            ? b with { InteractionKey = Guid.Parse(key) } : b).ToArray() };
        Assert.Contains("interactionKey", Assert.Throws<BlockError>(() => NpcBodyRig.Shape(Context, motions, "motions")).Field!, StringComparison.Ordinal);
    }

    [Fact]
    public void The_complete_27_binding_vector_is_canonical_and_timed_from_cooked_clips()
    {
        var a = NpcBodyRig.Bindings(Context, Motions(), "npc-body-test", Animation(), Graph(), (_, _) => 1.0000000005);
        var b = NpcBodyRig.Bindings(Context, Motions() with { Bindings = Motions().Bindings.Reverse().ToArray() }, "npc-body-test", Animation(), Graph(), (_, _) => 1.0000000005);
        Assert.Equal(27, a.Count);
        Assert.Equal(a.ToJsonString(), b.ToJsonString());
        Assert.All(a, row => Assert.Equal(1_000_000_001L, (long)row!["durationNs"]!));
        Assert.Equal(2, a.Count(row => (bool)row!["loop"]!));
    }

    [Fact]
    public void Optional_fallback_is_explicit_and_clamped_and_never_fills_required_motions()
    {
        var required = Motions() with { Bindings = Motions().Bindings.Where(b => NpcBodyRig.Required(b.Role)).ToArray(), OptionalFallback = AnimState.Idle };
        var vector = NpcBodyRig.Bindings(Context, required, "npc-body-test", Animation(), Graph(), (_, _) => 2);
        Assert.Equal(27, vector.Count);
        Assert.All(vector.Where(row => (bool)row!["clamp"]!), row => Assert.False((bool)row!["loop"]!));
        Assert.Throws<BlockError>(() => NpcBodyRig.Shape(Context, required with { OptionalFallback = null }, "motions"));
        var e = Assert.Throws<BlockError>(() => NpcBodyRig.Shape(Context, required with { Bindings = required.Bindings.Skip(1).ToArray() }, "motions"));
        Assert.Equal("motions.bindings", e.Field);
        Assert.Contains("required motions cannot fall back", e.Problem, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("role")]
    [InlineData("variant")]
    [InlineData("duplicate")]
    [InlineData("state")]
    [InlineData("interactionKey")]
    public void Bad_binding_shapes_name_their_field(string field)
    {
        var list = Motions().Bindings.ToList();
        list[0] = field switch
        {
            "role" => list[0] with { Role = (NpcMotionRole)999 },
            "variant" => list[0] with { Variant = 3 },
            "state" => list[0] with { State = null! },
            "interactionKey" => list[0] with { InteractionKey = Guid.NewGuid() },
            _ => list[0],
        };
        if (field == "duplicate") list.Add(list[0]);
        var e = Assert.Throws<BlockError>(() => NpcBodyRig.Shape(Context, Motions() with { Bindings = list }, "motions"));
        Assert.Contains(field == "duplicate" ? "bindings[27]" : field, e.Field!, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("Run", "GUID")]
    [InlineData("0xnot-a-state", "GUID")]
    [InlineData("absent", "not a state")]
    public void Motion_targets_must_exist_and_have_a_usable_GUID(string state, string problem)
    {
        var list = Motions().Bindings.ToArray();
        list[0] = list[0] with { State = new AnimState(state) };
        var e = Assert.Throws<BlockError>(() => NpcBodyRig.Bindings(Context, Motions() with { Bindings = list }, "npc-body-test", Animation(), Graph(), (_, _) => 1));
        Assert.Equal("motions.bindings[0].state", e.Field);
        Assert.Contains(problem, e.Problem, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    [InlineData(double.NaN)]
    [InlineData(double.PositiveInfinity)]
    public void Motion_durations_must_be_finite_and_positive(double seconds) =>
        Assert.Throws<BlockError>(() => NpcBodyRig.Bindings(Context, Motions(), "npc-body-test", Animation(), Graph(), (_, _) => seconds));

    [Fact]
    public void Sit_and_sleep_loop_contracts_are_checked_without_enemy_AI_lowering()
    {
        var animation = Animation();
        animation.States["Sit-1"] = new Clip("Hold") { Loop = false };
        var e = Assert.Throws<BlockError>(() => NpcBodyRig.Bindings(Context, Motions(), "npc-body-test", animation, Graph(), (_, _) => 1));
        Assert.Equal("motions.bindings[1].state", e.Field);
    }

    [Fact]
    public void Shared_rig_checks_keep_skeleton_and_slot_errors_field_specific()
    {
        var body = Body();
        NpcBodyRig.SkeletonNames(Context, "rig", "rig");
        NpcBodyRig.Cooked(Context, body, Graph(), Skeleton);
        Assert.Equal("animation", Assert.Throws<BlockError>(() => NpcBodyRig.SkeletonNames(Context, "body", "animation")).Field);
        Assert.Equal("body.model", Assert.Throws<BlockError>(() => NpcBodyRig.Cooked(Context, body, Graph(), Guid.NewGuid())).Field);
        var graph = Graph();
        graph.Slots.Remove("Head");
        Assert.Equal("body.slots[Head]", Assert.Throws<BlockError>(() => NpcBodyRig.Cooked(Context, body, graph, Skeleton)).Field);
    }

    [Theory]
    [InlineData("size")]
    [InlineData("size.radius")]
    [InlineData("size.height")]
    [InlineData("scale")]
    [InlineData("colliders")]
    [InlineData("template")]
    [InlineData("slots[Head]")]
    public void Roster_geometry_refuses_unsupported_input_with_the_field(string field)
    {
        var body = Body();
        body = field switch
        {
            "size" => body with { Size = null },
            "size.radius" => body with { Size = new BodySize(double.NaN, 2) },
            "size.height" => body with { Size = new BodySize(0.5, 9) },
            "scale" => body with { Scale = 2 },
            "colliders" => body with { Colliders = [Capsule.Of(0.5, 2)] },
            "template" => body with { Template = Bodies.Vukah.Template },
            _ => body with { Slots = new Dictionary<Slot, string> { [Slot.Root] = "root", [Slot.HandR] = "hand" } },
        };
        Assert.Equal($"body.{field}", Assert.Throws<BlockError>(() => NpcBodyRig.Geometry(Context, body)).Field);
    }

    [Fact]
    public void Bounds_fit_the_explicit_size_and_floor()
    {
        var box = JsonNode.Parse("""{"min":{"x":-0.5,"y":0,"z":-0.5},"max":{"x":0.5,"y":2,"z":0.5}}""")!;
        NpcBodyRig.Geometry(Context, Body(), box);
        box["min"]!["y"] = -0.1;
        Assert.Equal("body.size", Assert.Throws<BlockError>(() => NpcBodyRig.Geometry(Context, Body(), box)).Field);
    }

    [Fact]
    public void Ordinary_roster_lowering_keeps_its_existing_wire_bytes()
    {
        var mod = new ModBlocks("npc-plain", 1) { ManifestClients = "mod" };
        var clone = BlockIds.Derive(mod.Id, "npc", "tobin");
        var (_, low, _) = Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher });
        Assert.Equal("""{"template":"NPC_Workshop_Fisher_Trader","clone":{"guid":"GUID","name":"Kiln_npc-plain_tobin"},"names":{"en":"Tobin"}}""".Replace("GUID", clone.ToString(), StringComparison.Ordinal), low.Json["npc"]!.ToJsonString());
        Assert.Null(low.Json["npc"]!["body"]);
    }

    [Fact]
    public void Capability_row_follows_every_dependency_and_is_not_a_release_gate()
    {
        Capability Available(string name) => new(name, CapabilityState.Available, CapabilityReason.None, "", "test");
        Assert.Equal(CapabilityState.Available, BlockCapabilities.NpcBodyRow(true, Available).State);
        Assert.Equal(CapabilityReason.ServiceMissing, BlockCapabilities.NpcBodyRow(false, Available).Reason);
        foreach (var dependency in new[] { "quests.boot_pack", "quests.npc_body", "quests.npc", "assets.resources", "assets.content" })
        {
            var row = BlockCapabilities.NpcBodyRow(true, name => name == dependency ? new(name, CapabilityState.Unavailable, CapabilityReason.SymbolMissing, "layout changed", "test") : Available(name));
            Assert.Equal(CapabilityReason.SymbolMissing, row.Reason);
            Assert.Contains(dependency, row.Text!, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void Motions_are_a_strict_reusable_block_and_an_inline_NPC_part()
    {
        const string motions = """{"bindings":[{"role":"sit","variant":0,"state":"sit-enter"}],"optionalFallback":"Idle"}""";
        var npc = BlockJson.Parse<NpcBlock>("""{"name":"Tobin","body":{"model":"models/tobin.glb"},"motions":MOTIONS}""".Replace("MOTIONS", motions));
        var text = JsonSerializer.Serialize(npc, BlockJson.SharedOptions);
        Assert.Contains("\"motions\"", text, StringComparison.Ordinal);
        Assert.Contains("\"sit-enter\"", text, StringComparison.Ordinal);
        Assert.Contains(NpcJson.Kinds, k => k.Section == "npcMotions");
        Assert.Throws<JsonException>(() => BlockJson.Parse<NpcBlock>(text.Replace("\"variant\"", "\"variants\"")));
    }

    [Theory]
    [InlineData("{\"variant\":0,\"state\":\"sit-enter\"}")]
    [InlineData("{\"role\":\"sit\",\"state\":\"sit-enter\"}")]
    [InlineData("{\"role\":\"sit\",\"variant\":0}")]
    [InlineData("{\"role\":0,\"variant\":0,\"state\":\"sit-enter\"}")]
    public void JSON_bindings_require_named_role_variant_and_state(string binding) =>
        Assert.Throws<JsonException>(() => BlockJson.Parse<NpcMotionsBlock>("{\"bindings\":[" + binding + "]}"));

    [Fact]
    public void A_transaction_backend_without_body_lowering_does_not_make_authored_bodies_available()
    {
        var row = BlockCapabilities.NpcBodyRow(true, name => name == "quests.npc_body"
            ? new(name, CapabilityState.Unavailable, CapabilityReason.Unknown, "", "")
            : new(name, CapabilityState.Available, CapabilityReason.None, "", "test"));
        Assert.Equal(CapabilityState.Unavailable, row.State);
        Assert.Equal(CapabilityReason.ServiceMissing, row.Reason); // Unknown is a query result, never a reportable reason.
        Assert.Contains("quests.npc_body", row.Text!, StringComparison.Ordinal);
    }

    [Fact]
    public void An_unavailable_body_keeps_the_existing_diagnostic_and_names_the_missing_dependency()
    {
        var mod = new ModBlocks("body-unavailable", 1) { ManifestClients = "mod" };
        var e = Assert.Throws<BlockError>(() => Compiler.Define(mod, "tobin", new NpcBlock
        {
            Name = "Tobin", Body = new BodyBlock { Model = "tobin.glb", Size = new BodySize(0.75, 2) },
        }));
        Assert.Equal("body.model", e.Field);
        Assert.Contains("an NPC's body is a game NPC's for now", e.Problem, StringComparison.Ordinal);
        Assert.Contains("blocks.npc_body", e.Problem, StringComparison.Ordinal);
        Assert.Contains("quests.boot_pack", e.Problem, StringComparison.Ordinal);
        Assert.Empty(mod.Lowered);
    }
}
