// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using Json.Schema;
using Kiln;
using Xunit;
namespace CameraTests;

public sealed class SchemaTests
{
    private static string Repo => typeof(SchemaTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>().Single(a => a.Key == "KilnRepository").Value!;
    private static JsonSchema Schema()
    {
        var path = Path.Combine(Repo, "schema", "camera-override.schema.json");
        Assert.True(File.Exists(path), "camera authoring schema is missing");
        return JsonSchema.FromText(File.ReadAllText(path));
    }
    private static void Check(string json, bool valid) => Assert.Equal(valid, Schema().Evaluate(JsonNode.Parse(json)).IsValid);

    [Fact]
    public void Lens_defaults_and_mode_names_are_valid_shapes()
    {
        Check("""{"lens":{"fieldOfViewDegrees":40}}""", true);
        Check("""{"lens":{"fieldOfViewDegrees":160},"duration":120000,"priority":-1000,"blend":{"in":0,"out":100}}""", true);
        Check("""{"lens":{"fieldOfViewDegrees":40},"duration":0}""", true);
        foreach (var mode in Enum.GetNames<CameraMode>()) Check($$"""{"mode":{"mode":"{{mode}}"},"inputPolicy":"StockMode"}""", true);
    }
    [Fact]
    public void Schema_lists_the_complete_public_mode_enum_once()
    {
        _ = Schema();
        using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(Repo, "schema", "camera-override.schema.json")));
        Assert.Equal(Enum.GetNames<CameraMode>(), doc.RootElement.GetProperty("$defs").GetProperty("mode").GetProperty("properties").GetProperty("mode").GetProperty("enum").EnumerateArray().Select(x => x.GetString()));
    }
    [Theory]
    [InlineData("{}")]
    [InlineData("""{"lens":null}""")]
    [InlineData("""{"lens":{}}""")]
    [InlineData("""{"Lens":{"fieldOfViewDegrees":40}}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"duration":1.5}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"duration":120001}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"priority":1001}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"inputPolicy":"visualonly"}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":180}}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":0.5}}""")]
    [InlineData("""{"profile":{"follow":12,"distance":-1,"offsetLocal":[0,0,0]}}""")]
    [InlineData("""{"shake":{"amplitudeMetres":-0.5,"frequencyHz":2,"falloff":0}}""")]
    [InlineData("""{"path":{"keys":[{"at":0,"pose":{"position":[0,-1000001,0],"rotation":[0,0,0,1]}}]}}""")]
    [InlineData("""{"mode":{"mode":"PlayerCameraDefault"}}""")]
    [InlineData("""{"mode":{"mode":"PlayerCameraDefault"},"inputPolicy":"StockMode","lens":{"fieldOfViewDegrees":40}}""")]
    [InlineData("""{"mode":{"mode":"PlayerCameraDefault"},"inputPolicy":"StockMode","blend":{"in":1,"out":0}}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"inputPolicy":"StockMode"}""")]
    [InlineData("""{"profile":{"follow":12,"distance":20,"offsetLocal":[0,1,0],"lockRotation":true}}""")]
    [InlineData("""{"profile":{"follow":12,"distance":20,"offsetLocal":[0,0]}}""")]
    [InlineData("""{"profile":{"follow":12,"distance":20,"offsetLocal":[0,0,0],"collision":"Stock"}}""")]
    public void Invalid_shapes_are_refused(string json) => Check(json, false);
    [Fact]
    public void Profile_and_shake_keep_units_and_arrays()
    {
        Check("""{"profile":{"follow":12,"distance":20,"offsetLocal":[0,1,0],"offsetWorldY":2,"rotationMode":"FollowHeading","rotation":[0,0,0,1],"collision":"None"},"shake":{"amplitudeMetres":0.5,"frequencyHz":30,"falloff":100,"seed":4294967295}}""", true);
    }
    [Fact]
    public void Far_big_and_strong_values_are_valid_shapes()
    {
        // 0.2.0-dev.31.25: no distance or size caps beyond the world bound
        Check("""{"profile":{"follow":0,"distance":300,"offsetLocal":[0,0,150],"offsetWorldY":277,"rotationMode":"FixedWorld","rotation":[0.70710677,0,0,0.70710677]},"lens":{"fieldOfViewDegrees":170},"shake":{"amplitudeMetres":2,"frequencyHz":45,"falloff":0}}""", true);
        Check("""{"profile":{"follow":0,"distance":0,"offsetLocal":[0,0,0]},"shake":{"amplitudeMetres":0,"frequencyHz":0,"falloff":0}}""", true);
        Check("""{"path":{"keys":[{"at":0,"pose":{"position":[5000,300,-1000000],"rotation":[0,0,0,1]},"fieldOfViewDegrees":1},{"at":100,"pose":{"position":[0,300,0],"rotation":[0,0,0,1]},"fieldOfViewDegrees":179}]}}""", true);
    }
    [Fact]
    public void Path_lens_consistency_and_key_limits_match_the_codec()
    {
        const string key = """{"at":0,"pose":{"position":[1,2,3],"rotation":[0,0,0,1]}}""";
        Check("{\"path\":{\"keys\":[" + key + "]}}", true);
        Check("{\"path\":{\"keys\":[" + string.Join(',', Enumerable.Repeat(key, 257)) + "]}}", false);
        Check("""{"path":{"keys":[{"at":1,"pose":{"position":[1,2,3],"rotation":[0,0,0,1]}}]}}""", false);
        Check("""{"path":{"keys":[{"at":0,"pose":{"position":[1,2,3],"rotation":[0,0,0,1]},"fieldOfViewDegrees":40},{"at":100,"pose":{"position":[1,2,3],"rotation":[0,0,0,1]}}]}}""", false);
        Check("""{"path":{"keys":[{"at":0,"pose":{"position":[1,2,3],"rotation":[0,0,0,1]},"fieldOfViewDegrees":40}]},"lens":{"fieldOfViewDegrees":80}}""", false);
        Check("""{"path":{"keys":[{"at":0,"pose":{"position":[1,2,3],"rotation":[0,0,0,1]}}]},"profile":{"follow":0,"distance":10,"offsetLocal":[0,0,0]}}""", false);
    }
    [Fact]
    public void Authored_examples_validate_and_decode()
    {
        _ = Schema();
        var files = Directory.GetFiles(Path.Combine(Repo, "schema", "examples"), "camera-*.json");
        Assert.NotEmpty(files);
        foreach (var file in files) { var json = File.ReadAllText(file); Check(json, true); _ = CameraOverride.FromJson(json); }
    }
}
