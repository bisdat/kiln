// SPDX-License-Identifier: MIT
// Authoring JSON is the typed record, not the native ABI or transport blob. Read and written by hand over JsonElement and
// Utf8JsonWriter (no reflection, and no source generator: tools/ApiScan compiles the sources without generators):
// Numerics fields are arrays and never {}, enums are exact names, times are exact integer milliseconds.
using System.Buffers;
using System.Numerics;
using System.Text.Json;

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
            var request = CameraJson.Read(doc.RootElement);
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
        return CameraJson.Write(this);
    }
}

internal static class CameraJson
{
    internal static void Validate(CameraOverride request) => _ = CameraRequests.Build(request,
        request.Mode is null ? CameraEndpoint.Local : CameraEndpoint.ServerMode,
        request.Mode is null ? EntityId.None : new EntityId(1), "camera JSON");

    // ---- reading: every object lists its fields (an unknown one is refused, a computed one such as lockRotation too);
    // a required field must be there and not null; an optional one may be absent, and null where the record allows null

    internal static CameraOverride Read(JsonElement root)
    {
        Fields(root, "request", "mode", "profile", "path", "lens", "shake", "duration", "priority", "blend", "inputPolicy");
        return new CameraOverride(
            Mode: Maybe(root, "mode", ReadMode),
            Profile: Maybe(root, "profile", ReadProfile),
            Path: Maybe(root, "path", ReadPath),
            Lens: Maybe(root, "lens", l =>
            {
                Fields(l, "lens", "fieldOfViewDegrees");
                return new CameraLens(Float(Required(l, "fieldOfViewDegrees")));
            }),
            Shake: Maybe(root, "shake", ReadShake),
            Duration: Optional(root, "duration", Time, TimeSpan.Zero),
            Priority: Optional(root, "priority", Int, 0),
            Blend: Maybe(root, "blend", b =>
            {
                Fields(b, "blend", "in", "out");
                return new CameraBlend(Time(Required(b, "in")), Time(Required(b, "out")));
            }),
            InputPolicy: Optional(root, "inputPolicy", Name<CameraInputPolicy>, CameraInputPolicy.VisualOnly));
    }

    private static CameraModeRequest ReadMode(JsonElement m)
    {
        Fields(m, "mode", "mode", "transition");
        return new CameraModeRequest(Name<CameraMode>(Required(m, "mode")), Optional(m, "transition", Name<CameraTransitionOverride>, CameraTransitionOverride.None));
    }

    private static CameraProfile ReadProfile(JsonElement p)
    {
        Fields(p, "profile", "follow", "distance", "offsetLocal", "offsetWorldY", "rotationMode", "rotation", "collision");
        return new CameraProfile(Entity(Required(p, "follow")), Float(Required(p, "distance")), Vector(Required(p, "offsetLocal")),
            Optional(p, "offsetWorldY", Float, 0f), Optional(p, "rotationMode", Name<CameraRotationMode>, CameraRotationMode.FreeOrbit),
            MaybeValue(p, "rotation", Rotation), Optional(p, "collision", Name<CameraCollisionPolicy>, CameraCollisionPolicy.None));
    }

    private static CameraPath ReadPath(JsonElement p)
    {
        Fields(p, "path", "keys");
        var keys = Required(p, "keys");
        if (keys.ValueKind != JsonValueKind.Array || keys.GetArrayLength() is < 1 or > CameraNative.MaxKeys)
            throw Bad($"path.keys must have 1 to {CameraNative.MaxKeys} keys");
        var list = new List<CameraKey>(keys.GetArrayLength());
        foreach (var k in keys.EnumerateArray())
        {
            Fields(k, "key", "at", "pose", "fieldOfViewDegrees");
            var pose = Required(k, "pose");
            Fields(pose, "pose", "position", "rotation");
            list.Add(new CameraKey(Time(Required(k, "at")), new CameraPose(Position(Required(pose, "position")), Rotation(Required(pose, "rotation"))),
                MaybeValue(k, "fieldOfViewDegrees", Float)));
        }
        return new CameraPath(list);
    }

    private static CameraShake ReadShake(JsonElement s)
    {
        Fields(s, "shake", "amplitudeMetres", "frequencyHz", "falloff", "seed");
        return new CameraShake(Float(Required(s, "amplitudeMetres")), Float(Required(s, "frequencyHz")), Time(Required(s, "falloff")),
            Optional(s, "seed", e => e.ValueKind == JsonValueKind.Number && e.TryGetUInt32(out var v) ? v : throw Bad("seed must be an integer from 0 to 4294967295"), 0u));
    }

    private static ArgumentException Bad(string what) => new($"camera JSON: {what}", "json");

    private static void Fields(JsonElement value, string what, params string[] names)
    {
        if (value.ValueKind != JsonValueKind.Object) throw Bad($"{what} must be an object");
        foreach (var p in value.EnumerateObject())
            if (!names.Contains(p.Name, StringComparer.Ordinal)) throw Bad($"unknown field '{p.Name}'");
    }

    private static JsonElement Required(JsonElement parent, string name) =>
        parent.TryGetProperty(name, out var v) && v.ValueKind != JsonValueKind.Null ? v : throw Bad($"'{name}' is required");

    private static T Optional<T>(JsonElement parent, string name, Func<JsonElement, T> read, T absent) =>
        !parent.TryGetProperty(name, out var v) ? absent : v.ValueKind == JsonValueKind.Null ? throw Bad($"'{name}' must not be null") : read(v);

    private static T? Maybe<T>(JsonElement parent, string name, Func<JsonElement, T> read) where T : class =>
        parent.TryGetProperty(name, out var v) && v.ValueKind != JsonValueKind.Null ? read(v) : null;

    private static T? MaybeValue<T>(JsonElement parent, string name, Func<JsonElement, T> read) where T : struct =>
        parent.TryGetProperty(name, out var v) && v.ValueKind != JsonValueKind.Null ? read(v) : null;

    private static double Finite(JsonElement e) =>
        e.ValueKind == JsonValueKind.Number && e.TryGetDouble(out var d) && double.IsFinite(d) ? d : throw Bad("expected a finite number");

    private static float Float(JsonElement e) => Finite(e) is var d and >= -float.MaxValue and <= float.MaxValue ? (float)d : throw Bad("number exceeds float range");

    private static int Int(JsonElement e) => e.ValueKind == JsonValueKind.Number && e.TryGetInt32(out var v) ? v : throw Bad("expected a 32-bit integer");

    private static EntityId Entity(JsonElement e) =>
        e.ValueKind == JsonValueKind.Number && e.TryGetUInt32(out var id) ? new(id) : throw Bad("entity id must be a nonnegative uint32 integer");

    private static TimeSpan Time(JsonElement e) =>
        e.ValueKind == JsonValueKind.Number && e.TryGetInt64(out var ms) && ms is >= 0 and <= CameraNative.MaxDurationMs
            ? TimeSpan.FromTicks(ms * TimeSpan.TicksPerMillisecond) : throw Bad("time must be integer milliseconds from 0 to 120000");

    private static T Name<T>(JsonElement e) where T : struct, Enum =>
        e.ValueKind == JsonValueKind.String && e.GetString() is { } s && Enum.TryParse<T>(s, ignoreCase: false, out var value) && Enum.GetName(value) == s
            ? value : throw Bad($"expected an exact {typeof(T).Name} name");

    private static double[] Numbers(JsonElement e, int length)
    {
        if (e.ValueKind != JsonValueKind.Array || e.GetArrayLength() != length) throw Bad($"expected an array of {length} finite numbers");
        var values = new double[length];
        var i = 0;
        foreach (var item in e.EnumerateArray()) values[i++] = Finite(item);
        return values;
    }

    private static float F(double d) => d is >= -float.MaxValue and <= float.MaxValue ? (float)d : throw Bad("number exceeds float range");
    private static WorldPosition Position(JsonElement e) { var a = Numbers(e, 3); return new(a[0], a[1], a[2]); }
    private static Vector3 Vector(JsonElement e) { var a = Numbers(e, 3); return new(F(a[0]), F(a[1]), F(a[2])); }
    private static Quaternion Rotation(JsonElement e) { var a = Numbers(e, 4); return new(F(a[0]), F(a[1]), F(a[2]), F(a[3])); }

    // ---- writing: the record's own order, null optional fields left out

    internal static string Write(CameraOverride r)
    {
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            if (r.Mode is { } m)
            {
                w.WriteStartObject("mode");
                w.WriteString("mode", Name(m.Mode));
                w.WriteString("transition", Name(m.Transition));
                w.WriteEndObject();
            }
            if (r.Profile is { } p)
            {
                w.WriteStartObject("profile");
                w.WriteNumber("follow", p.Follow.Value);
                w.WriteNumber("distance", p.Distance);
                Floats(w, "offsetLocal", p.OffsetLocal.X, p.OffsetLocal.Y, p.OffsetLocal.Z);
                w.WriteNumber("offsetWorldY", p.OffsetWorldY);
                w.WriteString("rotationMode", Name(p.RotationMode));
                if (p.Rotation is { } q) Floats(w, "rotation", q.X, q.Y, q.Z, q.W);
                w.WriteString("collision", Name(p.Collision));
                w.WriteEndObject();
            }
            if (r.Path is { } path)
            {
                w.WriteStartObject("path");
                w.WriteStartArray("keys");
                foreach (var k in path.Keys)
                {
                    w.WriteStartObject();
                    w.WriteNumber("at", Ms(k.At));
                    w.WriteStartObject("pose");
                    w.WriteStartArray("position");
                    w.WriteNumberValue(k.Pose.Position.X);
                    w.WriteNumberValue(k.Pose.Position.Y);
                    w.WriteNumberValue(k.Pose.Position.Z);
                    w.WriteEndArray();
                    Floats(w, "rotation", k.Pose.Rotation.X, k.Pose.Rotation.Y, k.Pose.Rotation.Z, k.Pose.Rotation.W);
                    w.WriteEndObject();
                    if (k.FieldOfViewDegrees is { } fov) w.WriteNumber("fieldOfViewDegrees", fov);
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                w.WriteEndObject();
            }
            if (r.Lens is { } lens)
            {
                w.WriteStartObject("lens");
                w.WriteNumber("fieldOfViewDegrees", lens.FieldOfViewDegrees);
                w.WriteEndObject();
            }
            if (r.Shake is { } s)
            {
                w.WriteStartObject("shake");
                w.WriteNumber("amplitudeMetres", s.AmplitudeMetres);
                w.WriteNumber("frequencyHz", s.FrequencyHz);
                w.WriteNumber("falloff", Ms(s.Falloff));
                w.WriteNumber("seed", s.Seed);
                w.WriteEndObject();
            }
            w.WriteNumber("duration", Ms(r.Duration));
            w.WriteNumber("priority", r.Priority);
            if (r.Blend is { } b)
            {
                w.WriteStartObject("blend");
                w.WriteNumber("in", Ms(b.In));
                w.WriteNumber("out", Ms(b.Out));
                w.WriteEndObject();
            }
            w.WriteString("inputPolicy", Name(r.InputPolicy));
            w.WriteEndObject();
        }
        return System.Text.Encoding.UTF8.GetString(buffer.WrittenSpan);
    }

    private static void Floats(Utf8JsonWriter w, string name, params float[] values)
    {
        w.WriteStartArray(name);
        foreach (var v in values) w.WriteNumberValue(v);
        w.WriteEndArray();
    }

    private static long Ms(TimeSpan t) =>
        t.Ticks >= 0 && t.Ticks % TimeSpan.TicksPerMillisecond == 0 && t.TotalMilliseconds <= CameraNative.MaxDurationMs
            ? t.Ticks / TimeSpan.TicksPerMillisecond : throw Bad("time must be integer milliseconds from 0 to 120000");

    private static string Name<T>(T value) where T : struct, Enum => Enum.GetName(value) ?? throw Bad($"unknown {typeof(T).Name}");
}
