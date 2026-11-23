// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class BlockRangeTests
{
    private static string Repo([CallerFilePath] string self = "") => Path.GetFullPath(Path.Combine(Path.GetDirectoryName(self)!, "../../.."));
    private static readonly JsonObject Schema = JsonNode.Parse(File.ReadAllText(Path.Combine(Repo(), "schema/kiln-blocks.schema.json")))!.AsObject();
    private static readonly JsonArray Cases = JsonNode.Parse(File.ReadAllText(Path.Combine(Repo(), "schema/fixtures/block-ranges.json")))!.AsArray();

    public static IEnumerable<object[]> Fields() => Cases.Select(c => new object[] { (string)c!["id"]! });

    private static JsonNode Leaf(JsonNode c)
    {
        JsonNode node = Schema["$defs"]![(string)c["definition"]!]!;
        foreach (var part in ((string)c["schema"]!).Split('/', StringSplitOptions.RemoveEmptyEntries))
            node = part.StartsWith("$ref:", StringComparison.Ordinal) ? Schema["$defs"]![part[5..]]! : node is JsonArray a ? a[int.Parse(part)]! : node[part]!;
        return node;
    }

    private static void Set(JsonNode block, string path, double value, bool integer)
    {
        var parts = path.Split('.');
        for (var i = 0; i < parts.Length - 1; i++)
            block = block is JsonArray a ? a[int.Parse(parts[i])]! : block[parts[i]] ??= new JsonObject();
        block[parts[^1]] = integer ? JsonValue.Create((int)value) : JsonValue.Create(value);
    }

    private static bool Accepts(JsonNode bounds, double v) =>
        (bounds["minimum"] == null || v >= (double)bounds["minimum"]!) &&
        (bounds["exclusiveMinimum"] == null || v > (double)bounds["exclusiveMinimum"]!) &&
        (bounds["maximum"] == null || v <= (double)bounds["maximum"]!) &&
        (bounds["exclusiveMaximum"] == null || v < (double)bounds["exclusiveMaximum"]!);

    [Theory]
    [MemberData(nameof(Fields))]
    public void Schema_bounds_and_compiler_agree_at_and_around_each_boundary(string name)
    {
        var c = Cases.Single(c => (string)c!["id"]! == name)!;
        var leaf = Leaf(c);
        var bounds = c["bounds"]!;
        foreach (var key in new[] { "minimum", "exclusiveMinimum", "maximum", "exclusiveMaximum" })
            Assert.True(JsonNode.DeepEquals(bounds[key], leaf[key]), $"{name}: schema {key} {leaf[key]} != compiler contract {bounds[key]}");
        var integer = (string?)leaf["type"] == "integer";
        var step = integer ? 1 : .001;
        var values = bounds.AsObject().SelectMany(p => { var b = (double)p.Value!; var delta = integer ? 1 : Math.Max(step, Math.Abs(b) * .000001); return new[] { b - delta, b, b + delta }; }).Distinct();
        // The runtime column: a value outside the bounds that 0.2.0-dev.30.6 loaded still loads, with a warning, when it did no harm there.
        var warns = c["runtime"]?["warns"];
        foreach (var value in values)
        {
            var block = c["block"]!.DeepClone();
            Set(block, (string)c["field"]!, value, integer);
            IReadOnlyList<(string Field, string Problem)>? notes = null;
            var error = Record.Exception(() => notes = Compile(c, block));
            var expected = Accepts(bounds, value) ? "accepted" : warns != null && Accepts(warns, value) ? "warned" : "refused";
            Assert.True(expected == Reaction(error, notes), $"{name} = {value}: expected {expected}, got {Reaction(error, notes)} {error}{Notes(notes)}");
        }
    }

    private static string Reaction(Exception? error, IReadOnlyList<(string Field, string Problem)>? notes) => error != null ? "refused" : notes is { Count: > 0 } ? "warned" : "accepted";
    private static string Notes(IReadOnlyList<(string Field, string Problem)>? notes) => notes is { Count: > 0 } ? " " + string.Join("; ", notes.Select(n => $"{n.Field}: {n.Problem}")) : "";

    [Fact]
    public void Every_runtime_reaction_in_the_fixtures_says_why()
    {
        foreach (var c in Cases.Where(c => c!["runtime"] != null))
            Assert.False(string.IsNullOrWhiteSpace((string?)c!["runtime"]!["why"]), (string)c["id"]!);
        foreach (var c in Shapes.Where(c => c!["runtime"] != null))
        {
            Assert.Contains((string)c!["runtime"]!, new[] { "warns", "refuses" });
            Assert.False((bool)c["valid"]!, (string)c["name"]!);
            Assert.False(string.IsNullOrWhiteSpace((string?)c["why"]), (string)c["name"]!);
        }
    }

    private static readonly JsonArray Shapes = JsonNode.Parse(File.ReadAllText(Path.Combine(Repo(), "schema/fixtures/block-shapes.json")))!.AsArray();
    public static IEnumerable<object[]> ShapeNames() => Shapes.Select(c => new object[] { (string)c!["name"]! });

    [Theory]
    [MemberData(nameof(ShapeNames))]
    public void Shapes_and_relational_boundaries_match_lint_contract(string name)
    {
        var c = Shapes.Single(c => (string)c!["name"]! == name)!;
        IReadOnlyList<(string Field, string Problem)>? notes = null;
        var error = Record.Exception(() => notes = Compile(c, c["block"]!));
        // valid: loads clean; "runtime": "warns": loaded in 0.2.0-dev.30.6 and still loads, with a warning; anything else is refused
        var expected = (bool)c["valid"]! ? "accepted" : (string?)c["runtime"] == "warns" ? "warned" : "refused";
        Assert.True(expected == Reaction(error, notes), $"{name}: expected {expected}, got {Reaction(error, notes)} {error}{Notes(notes)}");
    }

    /// <summary>Compiles a fixture's block as the runtime does: what it tolerated (empty: nothing), or it throws.</summary>
    private static IReadOnlyList<(string Field, string Problem)> Compile(JsonNode c, JsonNode block)
    {
        var type = typeof(IBlock).Assembly.GetType("Kiln." + (string)c["type"]!, true)!;
        var parsed = block.Deserialize(type, BlockJson.SharedOptions)!;
        var mod = new ModBlocks("range-test", 1) { ManifestClients = "mod" };
        if ((string?)c["probe"] == "clip")
        {
            var ctx = new BlockContext(mod, "clip");
            try { typeof(NpcCompiler).GetMethod("CheckClip", BindingFlags.NonPublic | BindingFlags.Static)!.Invoke(null, [ctx, parsed, "clip"]); }
            catch (TargetInvocationException e) { ExceptionDispatchInfo.Capture(e.InnerException!).Throw(); }
            return ctx.Tolerated;
        }
        return Compiler.Lower(mod, "thing", (IBlock)parsed).Low.Tolerated;
    }

    [Fact]
    public void A_block_that_loaded_in_30_6_gets_one_warning_naming_every_field_once_per_load()
    {
        var mod = new ModBlocks("compat-test", 1) { ManifestClients = "mod" };
        var talent = new TalentBlock { Name = "Far", Column = 40, Row = -1, MaxLevel = 120 };
        Compiler.Define(mod, "far", talent);
        var warning = Assert.Single(mod.Warnings);
        Assert.StartsWith("block talent far: ", warning, StringComparison.Ordinal);
        foreach (var field in new[] { "column", "row", "maxLevel", "kiln lint", "0.2.0-dev.30.6" }) Assert.Contains(field, warning, StringComparison.Ordinal);
        Assert.True(mod.ReportDirty);
        // the same block again in this load: still one warning
        Compiler.Define(mod, "far", talent);
        Assert.Single(mod.Warnings);
        // a reload starts over: the block warns again, once
        mod.Reset();
        Assert.Empty(mod.Warnings);
        Compiler.Define(mod, "far", talent);
        Assert.Single(mod.Warnings);
        // a clean block warns nothing
        Compiler.Define(mod, "near", new TalentBlock { Name = "Near", Column = 1, Row = 1 });
        Assert.Single(mod.Warnings);
    }

    [Fact]
    public void A_refused_block_leaves_no_compat_warning_behind()
    {
        var mod = new ModBlocks("compat-refused", 1) { ManifestClients = "mod" };
        // column 40 alone would warn; a talent of the game's tree with a grid cell is refused, as in 0.2.0-dev.30.6
        var game = new TalentBlock { Name = "Node", Column = 40, Game = new GameTreePlace { Class = "Warrior", Position = [0, 0] } };
        Assert.Equal("column", Assert.Throws<BlockError>(() => Compiler.Define(mod, "node", game)).Field);
        Assert.Empty(mod.Warnings);
    }

    [Fact]
    public void A_game_sequence_attack_loads_its_unused_length_and_hit_time_as_30_6_did()
    {
        // 0.2.0-dev.30.6 returned for a game sequence before it checked length and hitAt (a sequence plays as it is and uses
        // neither): such an attack still loads, with the block's one warning naming them
        var mod = new ModBlocks("compat-sequence", 1) { ManifestClients = "mod" };
        static AttackBlock Attack(string json) => JsonNode.Parse(json)!.Deserialize<AttackBlock>(BlockJson.SharedOptions)!;
        const string seq = "\"sequence\": \"00000000-0000-0000-0000-000000000001\"";
        var low = Compiler.Lower(mod, "seq", Attack($"{{{seq}, \"length\": 0.3}}")).Low;
        Assert.Equal("hitAt", Assert.Single(low.Tolerated).Field);
        low = Compiler.Lower(mod, "seq", Attack($"{{{seq}, \"length\": 0, \"hitAt\": -1}}")).Low;
        Assert.Equal(["length", "hitAt"], low.Tolerated.Select(t => t.Field));
        Assert.Empty(Compiler.Lower(mod, "seq", Attack($"{{{seq}}}")).Low.Tolerated);
        // an attack of the mod's own still refuses them: its hit happens at hitAt within length
        Assert.Equal("hitAt", Assert.Throws<BlockError>(() =>
            Compiler.Lower(mod, "own", Attack("{\"plays\": \"Idle\", \"onHit\": [{\"heal\": 1}], \"length\": 0.3}"))).Field);
    }

    [Fact]
    public void Stagger_on_the_games_graph_is_ignored_as_30_6_did()
    {
        var mod = new ModBlocks("compat-stagger", 1) { ManifestClients = "mod" };
        var plain = Compiler.Lower(mod, "plain", new AnimationBlock { Template = "Enemy_Vukah_Melee" }).Low;
        var staggered = Compiler.Lower(mod, "plain", new AnimationBlock { Template = "Enemy_Vukah_Melee", Stagger = new Clip { Name = "Stagger" } }).Low;
        Assert.True(JsonNode.DeepEquals(plain.Json, staggered.Json));
        Assert.Empty(plain.Tolerated);
        Assert.Equal("stagger", Assert.Single(staggered.Tolerated).Field);
    }

    [Fact]
    public void A_sphere_collider_ignores_a_height_below_zero_as_30_6_did()
    {
        var mod = new ModBlocks("compat-sphere", 1) { ManifestClients = "mod" };
        var low = Compiler.Lower(mod, "ball", new BodyBlock { Model = "body.glb", Colliders = [new ColliderBlock { Shape = "sphere", Radius = 1, Height = -3 }] }).Low;
        Assert.Equal("colliders[0].height", Assert.Single(low.Tolerated).Field);
    }

    [Fact]
    public void A_creature_still_refuses_a_rule_that_a_standalone_rule_only_warns_about()
    {
        var mod = new ModBlocks("compat-rule", 1) { ManifestClients = "mod" };
        var rule = new RuleBlock { Every = 0, Do = [Act.SetPhase("hurt")] };
        Assert.Equal("every", Assert.Single(Compiler.Lower(mod, "lonely", rule).Low.Tolerated).Field);
        var creature = new CreatureBlock { Body = Bodies.Vukah, Behaviour = new BehaviourBlock { Rules = [rule] } };
        Assert.Equal("behaviour.rules[0].every", Assert.Throws<BlockError>(() => Compiler.Lower(mod, "uses-it", creature)).Field);
    }

    [Theory]
    [InlineData(179)]
    [InlineData(180)]
    [InlineData(359)]
    [InlineData(360)]
    public void Attack_aperture_is_honoured_without_clamping(double angle)
    {
        var creature = new CreatureBlock
        {
            Body = Bodies.Vukah,
            Behaviour = new BehaviourBlock { Attacks = [new AttackBlock { Name = "sweep", Plays = AnimState.Idle, Angle = angle, OnHit = [new EffectBlock { Heal = 1 }] }] },
        };
        var mod = new ModBlocks("angle-test", 1) { ManifestClients = "mod" };
        Compiler.Lower(mod, "sweeper", creature);
        var listing = string.Join("\n", mod.Programs.Values.Select(p => p.Listing));
        // Every directional hit has a share constant; 360 deliberately has no directional filter.
        Assert.Equal(angle != 360, listing.Contains("share", StringComparison.Ordinal));
        if (angle != 360)
        {
            var share = BitConverter.GetBytes(2 * MathF.Sin((float)angle * MathF.PI / 180 / 4));
            Assert.Contains(mod.Programs.Values, p => p.Blob.AsSpan().IndexOf(share) >= 0);
        }
    }

    [Fact]
    public void An_attack_with_a_length_of_zero_is_refused_for_its_length()
    {
        var mod = new ModBlocks("length-test", 1) { ManifestClients = "mod" };
        var error = Assert.Throws<BlockError>(() => Compiler.Lower(mod, "slam", new AttackBlock { Plays = AnimState.Idle, Length = 0, OnHit = [new EffectBlock { Heal = 1 }] }));
        Assert.Equal("length", error.Field);
        Assert.Contains("more than 0 and at most 30", error.Problem, StringComparison.Ordinal);
    }

    [Fact]
    public void An_angle_that_is_not_a_number_is_refused_rather_than_reaching_all_around()
    {
        var mod = new ModBlocks("nan-test", 1) { ManifestClients = "mod" };
        var attack = new AttackBlock { Plays = AnimState.Idle, Angle = double.NaN, OnHit = [new EffectBlock { Heal = 1 }] };
        Assert.Equal("angle", Assert.Throws<BlockError>(() => Compiler.Lower(mod, "slam", attack)).Field);
        var cone = new AbilityBlock { Name = "Cone", Delivery = new ConeBlock { Range = 8, Angle = float.NaN, Effects = [new EffectBlock { Heal = 1 }] } };
        Assert.Equal("delivery.angle", Assert.Throws<BlockError>(() => Compiler.Lower(mod, "cone", cone)).Field);
    }
}
