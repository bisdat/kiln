// SPDX-License-Identifier: MIT
using System.Numerics;
using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Text.Json;
using Kiln;
using Xunit;
namespace CameraTests;

public sealed class JsonTests
{
    // Reflection lets the red-first test run against the pre-JSON author surface (rather than fail to compile).
    private static object? Invoke(MethodInfo? method, object? target, params object?[] args)
    {
        Assert.NotNull(method);
        try { return method.Invoke(target, args); }
        catch (TargetInvocationException e) when (e.InnerException is not null)
        { ExceptionDispatchInfo.Capture(e.InnerException).Throw(); throw; }
    }
    private static CameraOverride Read(string json) => (CameraOverride)Invoke(typeof(CameraOverride).GetMethod("FromJson", [typeof(string)]), null, json)!;
    private static string Write(CameraOverride request) => (string)Invoke(typeof(CameraOverride).GetMethod("ToJson", Type.EmptyTypes), request)!;

    [Fact]
    public void Lens_json_is_the_typed_record_in_camelCase_and_integer_milliseconds()
    {
        var request = new CameraOverride(Lens: new CameraLens(40), Duration: TimeSpan.FromMilliseconds(1500), Blend: new(TimeSpan.FromMilliseconds(10), TimeSpan.FromMilliseconds(20)));
        var json = Write(request);
        using var doc = JsonDocument.Parse(json);
        Assert.Equal(1500, doc.RootElement.GetProperty("duration").GetInt32());
        Assert.Equal(10, doc.RootElement.GetProperty("blend").GetProperty("in").GetInt32());
        Assert.Equal("VisualOnly", doc.RootElement.GetProperty("inputPolicy").GetString());
        Assert.Equal(request, Read(json));
    }
    [Fact]
    public void Path_positions_and_quaternions_are_arrays_not_empty_numerics_objects()
    {
        var json = """{"path":{"keys":[{"at":0,"pose":{"position":[1.25,2,3],"rotation":[0,0,0,1]},"fieldOfViewDegrees":40},{"at":500,"pose":{"position":[4,5,6],"rotation":[0,0,0,1]},"fieldOfViewDegrees":80}]},"duration":1000}""";
        var request = Read(json);
        Assert.Equal(new WorldPosition(1.25, 2, 3), request.Path!.Keys[0].Pose.Position);
        Assert.Equal(Quaternion.Identity, request.Path.Keys[1].Pose.Rotation);
        Assert.Equal(80, request.Path.Keys[1].FieldOfViewDegrees);
        using var doc = JsonDocument.Parse(Write(request));
        Assert.Equal(3, doc.RootElement.GetProperty("path").GetProperty("keys")[0].GetProperty("pose").GetProperty("position").GetArrayLength());
        Assert.Equal(Write(request), Write(Read(Write(request))));
    }
    [Fact]
    public void Profile_vectors_ids_optional_rotation_and_shake_round_trip()
    {
        var request = new CameraOverride(Profile: new(new EntityId(123), 12, new Vector3(1,2,3), 4, CameraRotationMode.FollowHeading, Quaternion.Identity),
            Shake: new(0.2f, 3, TimeSpan.FromMilliseconds(300), uint.MaxValue), Duration: TimeSpan.FromMilliseconds(2000));
        var json = Write(request);
        Assert.DoesNotContain("lockRotation", json);
        Assert.Equal(request, Read(json));
        Assert.Equal(request with { Profile = request.Profile! with { Rotation = null } }, Read(Write(request with { Profile = request.Profile! with { Rotation = null } })));
    }
    [Fact]
    public void Mode_names_are_exact_and_no_world_or_capability_is_needed()
    {
        var request = new CameraOverride(Mode: new(CameraMode.PlayerCameraBuildingFirstPerson, CameraTransitionOverride.CameraTransitionLerpSpeed), InputPolicy: CameraInputPolicy.StockMode);
        Assert.Equal(request, Read(Write(request)));
        Assert.Contains("PlayerCameraBuildingFirstPerson", Write(request));
        Assert.Equal(TimeSpan.Zero, Read("""{"lens":{"fieldOfViewDegrees":40}}""").Duration); // same default as the authored record: ten seconds at dispatch
    }
    [Fact]
    public void JsonElement_can_be_read_from_a_larger_authoring_document()
    {
        using var doc = JsonDocument.Parse("""{"camera":{"lens":{"fieldOfViewDegrees":40}}}""");
        var method = typeof(CameraOverride).GetMethod("FromJson", [typeof(JsonElement)]);
        var value = (CameraOverride)Invoke(method, null, doc.RootElement.GetProperty("camera"))!;
        Assert.Equal(new CameraLens(40), value.Lens);
    }
    [Theory]
    [InlineData("{}")]
    [InlineData("null")]
    [InlineData("[]")]
    [InlineData("{not json}")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"typo":true}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"Lens":{"fieldOfViewDegrees":80}}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40,"fieldOfViewDegrees":80}}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"duration":1.5}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"duration":"00:00:01"}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"inputPolicy":"visualonly"}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"inputPolicy":0}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":40},"inputPolicy":"0"}""")]
    [InlineData("""{"lens":{}}""")]
    [InlineData("""{"lens":{"fieldOfViewDegrees":161}}""")]
    [InlineData("""{"mode":{"mode":"PlayerCameraBuildingFirstPerson"},"inputPolicy":"VisualOnly"}""")]
    [InlineData("""{"profile":{"follow":1,"distance":10,"offsetLocal":[0,0,0],"lockRotation":true}}""")]
    [InlineData("""{"profile":{"follow":1,"distance":10,"offsetLocal":[0,0]}}""")]
    [InlineData("""{"profile":{"follow":1,"distance":10,"offsetLocal":[0,0,0],"rotation":[0,0,0,0]}}""")]
    [InlineData("""{"profile":{"follow":-1,"distance":10,"offsetLocal":[0,0,0]}}""")]
    [InlineData("""{"path":{"keys":[{"at":0,"pose":{"position":[0,0,0],"rotation":[0,0,0,1]}},{"at":1,"pose":{"position":[0,0,0],"rotation":[0,0,0,1]},"fieldOfViewDegrees":40}]}}""")]
    public void Invalid_json_is_an_argument_error_before_any_native_access(string json) => Assert.Throws<ArgumentException>(() => Read(json));
    // tools/ApiScan (gen-apis.sh) compiles Kiln.Camera's sources without source generators: a JsonSerializerContext
    // would have no generated half there (CS0534), and the analyzer table could not be regenerated.
    [Fact]
    public void The_codec_needs_no_source_generator()
    {
        var generated = typeof(CameraOverride).Assembly.GetTypes().Where(t => t.IsSubclassOf(typeof(System.Text.Json.Serialization.JsonSerializerContext)));
        Assert.Empty(generated);
    }

    [Fact]
    public void Null_where_the_record_allows_it_is_absent_and_elsewhere_refused()
    {
        Assert.Equal(new CameraOverride(Lens: new CameraLens(40)), Read("""{"lens":{"fieldOfViewDegrees":40},"shake":null,"blend":null}"""));
        Assert.Throws<ArgumentException>(() => Read("""{"lens":{"fieldOfViewDegrees":40},"duration":null}"""));
        Assert.Throws<ArgumentException>(() => Read("""{"lens":{"fieldOfViewDegrees":null}}"""));
        var profile = Read("""{"profile":{"follow":0,"distance":8,"offsetLocal":[0,1,0],"rotation":null}}""");
        Assert.Null(profile.Profile!.Rotation);
        Assert.Throws<ArgumentException>(() => Read("""{"profile":{"follow":0,"distance":8,"offsetLocal":[0,1,0],"collision":null}}"""));
    }

    [Fact]
    public void Writing_does_not_round_fractional_milliseconds_or_invalid_values()
    {
        Assert.Throws<ArgumentException>(() => Write(new(Lens: new(40), Duration: TimeSpan.FromTicks(15000))));
        Assert.Throws<ArgumentException>(() => Write(new(Lens: new(float.NaN))));
    }
}
