// SPDX-License-Identifier: MIT
// Authoring JSON is the typed record, not the native ABI or transport blob. All serialization uses generated
// metadata and explicit converters: Numerics fields must never become {}, and times are exact integer milliseconds.
using System.Numerics;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

public sealed partial record CameraOverride
{
    /// <summary>Reads an authored request: camelCase fields, exact enum names, position/vector/quaternion arrays,
    /// and integer milliseconds. Absent members have the same defaults as the C# record; duration zero means ten seconds.
    /// Unknown/duplicate fields, malformed shapes and invalid requests are refused. No world or capability is needed.</summary>
    /// <exception cref="ArgumentException">The JSON or request is invalid.</exception>
    [AnyThread]
    public static CameraOverride FromJson(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        try
        {
            using var doc = JsonDocument.Parse(json, new JsonDocumentOptions { AllowDuplicateProperties = false });
            CameraJson.CheckShape(doc.RootElement);
            var request = JsonSerializer.Deserialize(json, CameraJsonContext.Default.CameraOverride)
                ?? throw new ArgumentException("camera JSON must be a request object", nameof(json));
            CameraJson.Validate(request);
            return request;
        }
        catch (JsonException e) { throw new ArgumentException($"camera JSON: {e.Message}", nameof(json), e); }
    }

    /// <summary>Reads one request object in a larger authoring document, with the rules of <see cref="FromJson(string)"/>.</summary>
    /// <exception cref="ArgumentException">The JSON or request is invalid.</exception>
    [AnyThread]
    public static CameraOverride FromJson(JsonElement json) => FromJson(json.GetRawText());

    /// <summary>Writes this request in the format <see cref="FromJson(string)"/> reads, after validating it.
    /// Null optional fields are omitted. A zero duration remains zero (the authored ten-second default).</summary>
    /// <exception cref="ArgumentException">This request is invalid, including a non-integral millisecond time.</exception>
    [AnyThread]
    public string ToJson()
    {
        CameraJson.Validate(this);
        return JsonSerializer.Serialize(this, CameraJsonContext.Default.CameraOverride);
    }
}

internal static class CameraJson
{
    internal static void Validate(CameraOverride request) => _ = CameraRequests.Build(request,
        request.Mode is null ? CameraEndpoint.Local : CameraEndpoint.ServerMode,
        request.Mode is null ? EntityId.None : new EntityId(1), "camera JSON");

    // Keep ignored computed properties out of the input too (JsonSerializer otherwise ignores LockRotation).
    internal static void CheckShape(JsonElement root)
    {
        Only(root, "mode", "profile", "path", "lens", "shake", "duration", "priority", "blend", "inputPolicy");
        if (Object(root, "mode", out var mode)) Only(mode, "mode", "transition");
        if (Object(root, "profile", out var profile)) Only(profile, "follow", "distance", "offsetLocal", "offsetWorldY", "rotationMode", "rotation", "collision");
        if (Object(root, "lens", out var lens)) Only(lens, "fieldOfViewDegrees");
        if (Object(root, "shake", out var shake)) Only(shake, "amplitudeMetres", "frequencyHz", "falloff", "seed");
        if (Object(root, "blend", out var blend)) Only(blend, "in", "out");
        if (!Object(root, "path", out var path)) return;
        Only(path, "keys");
        if (!path.TryGetProperty("keys", out var keys) || keys.ValueKind != JsonValueKind.Array || keys.GetArrayLength() is < 1 or > CameraNative.MaxKeys)
            throw new ArgumentException($"camera JSON: path.keys must have 1 to {CameraNative.MaxKeys} keys", "json");
        foreach (var key in keys.EnumerateArray())
        {
            Only(key, "at", "pose", "fieldOfViewDegrees");
            if (Object(key, "pose", out var pose)) Only(pose, "position", "rotation");
        }
    }
    private static bool Object(JsonElement parent, string name, out JsonElement value) =>
        parent.TryGetProperty(name, out value) && value.ValueKind != JsonValueKind.Null;
    private static void Only(JsonElement value, params string[] names)
    {
        if (value.ValueKind != JsonValueKind.Object) throw new ArgumentException("camera JSON: expected an object", "json");
        foreach (var p in value.EnumerateObject())
            if (!names.Contains(p.Name, StringComparer.Ordinal)) throw new ArgumentException($"camera JSON: unknown field '{p.Name}'", "json");
    }
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    GenerationMode = JsonSourceGenerationMode.Metadata, DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    IgnoreReadOnlyProperties = true, UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
    RespectRequiredConstructorParameters = true, RespectNullableAnnotations = true, AllowDuplicateProperties = false,
    Converters = [typeof(CameraPositionJson), typeof(CameraVectorJson), typeof(CameraQuaternionJson), typeof(CameraEntityJson), typeof(CameraTimeJson),
        typeof(CameraModeJson), typeof(CameraTransitionJson), typeof(CameraInputJson), typeof(CameraRotationJson), typeof(CameraCollisionJson)])]
[JsonSerializable(typeof(CameraOverride))]
internal partial class CameraJsonContext : JsonSerializerContext;

internal static class CameraJsonArray
{
    internal static double[] Read(ref Utf8JsonReader reader, int length)
    {
        if (reader.TokenType != JsonTokenType.StartArray) throw new JsonException($"expected an array of {length} finite numbers");
        var values = new double[length];
        for (var i = 0; i < length; i++)
            if (!reader.Read() || reader.TokenType != JsonTokenType.Number || !reader.TryGetDouble(out values[i]) || !double.IsFinite(values[i]))
                throw new JsonException($"expected an array of {length} finite numbers");
        if (!reader.Read() || reader.TokenType != JsonTokenType.EndArray) throw new JsonException($"expected exactly {length} numbers");
        return values;
    }
    internal static float Float(double d) => d is >= -float.MaxValue and <= float.MaxValue ? (float)d : throw new JsonException("number exceeds float range");
}
internal sealed class CameraPositionJson : JsonConverter<WorldPosition>
{
    public override WorldPosition Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) { var a = CameraJsonArray.Read(ref r, 3); return new(a[0], a[1], a[2]); }
    public override void Write(Utf8JsonWriter w, WorldPosition p, JsonSerializerOptions o) { w.WriteStartArray(); w.WriteNumberValue(p.X); w.WriteNumberValue(p.Y); w.WriteNumberValue(p.Z); w.WriteEndArray(); }
}
internal sealed class CameraVectorJson : JsonConverter<Vector3>
{
    public override Vector3 Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) { var a = CameraJsonArray.Read(ref r, 3); return new(CameraJsonArray.Float(a[0]), CameraJsonArray.Float(a[1]), CameraJsonArray.Float(a[2])); }
    public override void Write(Utf8JsonWriter w, Vector3 p, JsonSerializerOptions o) { w.WriteStartArray(); w.WriteNumberValue(p.X); w.WriteNumberValue(p.Y); w.WriteNumberValue(p.Z); w.WriteEndArray(); }
}
internal sealed class CameraQuaternionJson : JsonConverter<Quaternion>
{
    public override Quaternion Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) { var a = CameraJsonArray.Read(ref r, 4); return new(CameraJsonArray.Float(a[0]), CameraJsonArray.Float(a[1]), CameraJsonArray.Float(a[2]), CameraJsonArray.Float(a[3])); }
    public override void Write(Utf8JsonWriter w, Quaternion q, JsonSerializerOptions o) { w.WriteStartArray(); w.WriteNumberValue(q.X); w.WriteNumberValue(q.Y); w.WriteNumberValue(q.Z); w.WriteNumberValue(q.W); w.WriteEndArray(); }
}
internal sealed class CameraEntityJson : JsonConverter<EntityId>
{
    public override EntityId Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) => r.TokenType == JsonTokenType.Number && r.TryGetUInt32(out var id) ? new(id) : throw new JsonException("entity id must be a nonnegative uint32 integer");
    public override void Write(Utf8JsonWriter w, EntityId id, JsonSerializerOptions o) => w.WriteNumberValue(id.Value);
}
internal sealed class CameraTimeJson : JsonConverter<TimeSpan>
{
    public override TimeSpan Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) =>
        r.TokenType == JsonTokenType.Number && r.TryGetInt64(out var ms) && ms is >= 0 and <= CameraNative.MaxDurationMs
            ? TimeSpan.FromTicks(ms * TimeSpan.TicksPerMillisecond) : throw new JsonException("time must be integer milliseconds from 0 to 120000");
    public override void Write(Utf8JsonWriter w, TimeSpan value, JsonSerializerOptions o)
    {
        if (value.Ticks < 0 || value.Ticks % TimeSpan.TicksPerMillisecond != 0 || value.TotalMilliseconds > CameraNative.MaxDurationMs) throw new JsonException("time must be integer milliseconds from 0 to 120000");
        w.WriteNumberValue(value.Ticks / TimeSpan.TicksPerMillisecond);
    }
}
internal abstract class CameraEnumJson<T> : JsonConverter<T> where T : struct, Enum
{
    public override T Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) =>
        r.TokenType == JsonTokenType.String && Enum.TryParse<T>(r.GetString(), ignoreCase: false, out var value) && Enum.GetName(value) == r.GetString()
            ? value : throw new JsonException($"expected an exact {typeof(T).Name} name");
    public override void Write(Utf8JsonWriter w, T value, JsonSerializerOptions o) => w.WriteStringValue(Enum.GetName(value) ?? throw new JsonException($"unknown {typeof(T).Name}"));
}
internal sealed class CameraModeJson : CameraEnumJson<CameraMode>;
internal sealed class CameraTransitionJson : CameraEnumJson<CameraTransitionOverride>;
internal sealed class CameraInputJson : CameraEnumJson<CameraInputPolicy>;
internal sealed class CameraRotationJson : CameraEnumJson<CameraRotationMode>;
internal sealed class CameraCollisionJson : CameraEnumJson<CameraCollisionPolicy>;
