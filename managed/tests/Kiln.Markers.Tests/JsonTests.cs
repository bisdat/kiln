// SPDX-License-Identifier: MIT
// MarkerSpec's JSON (chapter 04 §5; G6 of the dev.28 plan: the converter lives in Kiln.Markers): the same defaults as C#,
// strict field names and types, null for no icon and no follow, and a round trip.
using System.Text.Json;
using Kiln;
using Xunit;

namespace MarkersTests;

public sealed class JsonTests : MarkersFixture
{
    private const string World = """{"id":"camp","position":{"x":3800,"y":905,"z":1100},"text":"Camp","icon":null,"surfaces":["world","compass"],"follow":null,"height":2,"maxDistance":250.5,"priority":-3,"rgba":2164195583,"mapLabel":"none"}""";

    [Fact]
    public void A_compass_and_world_marker_reads_without_a_catalog()
    {
        var spec = MarkerSpec.FromJson(World);
        Assert.Equal(new MarkerSpec("camp", new WorldPosition(3800, 905, 1100), "Camp", MarkerIcon.None, MarkerSurface.World | MarkerSurface.Compass,
            null, 2, 250.5f, -3, 0x80ff00ff, MarkerMapLabel.None), spec);
        Assert.Equal(spec, MarkerSpec.FromJson(spec.ToJson()));
        using var doc = JsonDocument.Parse("[" + World + "]");
        Assert.Equal(spec, MarkerSpec.FromJson(doc.RootElement[0]));
    }

    [Fact]
    public void Absent_fields_take_the_same_defaults_as_CSharp()
    {
        var spec = MarkerSpec.FromJson("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":""}""");
        Assert.Equal(new MarkerSpec("k", new WorldPosition(1, 2, 3), "", MarkerIcon.None), spec);
        Assert.Equal(MarkerSurface.Map | MarkerSurface.Compass, spec.Surfaces);
        Assert.Equal((0f, 0f, 0, 0xffffffffu, MarkerMapLabel.None, null as MarkerAnchor), (spec.Height, spec.MaxDistance, spec.Priority, spec.Rgba, spec.MapLabel, spec.Follow));
        Assert.Equal(MarkerMapLabel.NativeTooltip, MarkerSpec.FromJson("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","mapLabel":"nativeTooltip"}""").MapLabel);
        var json = spec.ToJson();
        Assert.Contains("\"surfaces\":[\"map\",\"compass\"]", json);
        Assert.Contains("\"icon\":null", json);
        Assert.Contains("\"follow\":null", json);
        Assert.Contains("\"rgba\":4294967295", json);
    }

    [Theory]
    [InlineData("[]")]
    [InlineData("""{"position":{"x":1,"y":2,"z":3},"text":""}""")]
    [InlineData("""{"id":"k","text":""}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3}}""")]
    [InlineData("""{"id":5,"position":{"x":1,"y":2,"z":3},"text":""}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2},"text":""}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3,"w":4},"text":""}""")]
    [InlineData("""{"id":"k","position":[1,2,3],"text":""}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","colour":1}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","surfaces":[]}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","surfaces":["radar"]}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","surfaces":"map"}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","priority":1.5}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","priority":3000000000}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","rgba":-1}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","rgba":4294967296}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","height":"2"}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","height":1e40}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","mapLabel":"tooltip"}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","icon":7}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","follow":7}""")]
    [InlineData("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","follow":{"entity":7}}""")]
    [InlineData("""not json""")]
    public void A_malformed_marker_is_an_argument_exception(string json) =>
        Assert.ThrowsAny<ArgumentException>(() => MarkerSpec.FromJson(json));

    [Fact]
    public void A_follow_lease_in_JSON_needs_the_anchors()
    {
        var e = Assert.Throws<KilnUnavailableException>(() =>
            MarkerSpec.FromJson("""{"id":"k","position":{"x":1,"y":2,"z":3},"text":"","follow":{"anchor":"0000000000001234"}}"""));
        Assert.Equal("markers.anchors.client", e.Capability);
        Assert.Throws<ArgumentNullException>(() => MarkerSpec.FromJson((string)null!));
    }
}
