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
        foreach (var value in values)
        {
            var block = c["block"]!.DeepClone();
            Set(block, (string)c["field"]!, value, integer);
            var error = Record.Exception(() => Compile(c, block));
            Assert.True(Accepts(bounds, value) == (error == null), $"{name} = {value}: expected {(Accepts(bounds, value) ? "accepted" : "refused")}, got {error}");
        }
    }

    private static readonly JsonArray Shapes = JsonNode.Parse(File.ReadAllText(Path.Combine(Repo(), "schema/fixtures/block-shapes.json")))!.AsArray();
    public static IEnumerable<object[]> ShapeNames() => Shapes.Select(c => new object[] { (string)c!["name"]! });

    [Theory]
    [MemberData(nameof(ShapeNames))]
    public void Shapes_and_relational_boundaries_match_lint_contract(string name)
    {
        var c = Shapes.Single(c => (string)c!["name"]! == name)!;
        var error = Record.Exception(() => Compile(c, c["block"]!));
        Assert.True((bool)c["valid"]! == (error == null), $"{name}: {error}");
    }

    private static void Compile(JsonNode c, JsonNode block)
    {
        var type = typeof(IBlock).Assembly.GetType("Kiln." + (string)c["type"]!, true)!;
        var parsed = block.Deserialize(type, BlockJson.SharedOptions)!;
        var mod = new ModBlocks("range-test", 1) { ManifestClients = "mod" };
        if ((string?)c["probe"] == "clip")
        {
            try { typeof(NpcCompiler).GetMethod("CheckClip", BindingFlags.NonPublic | BindingFlags.Static)!.Invoke(null, [new BlockContext(mod, "clip"), parsed, "clip"]); }
            catch (TargetInvocationException e) { ExceptionDispatchInfo.Capture(e.InnerException!).Throw(); }
        }
        else Compiler.Lower(mod, "thing", (IBlock)parsed);
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
}
