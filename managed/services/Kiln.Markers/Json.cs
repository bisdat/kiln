// SPDX-License-Identifier: MIT
// MarkerSpec in JSON (chapter 04 §5): the same fields, defaults and units as C# and the native ABI. A typed record first;
// this converter is for authoring files and runtime JSON. Icons by canonical key or alias through the icon catalog.
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Kiln;

public sealed partial record MarkerSpec
{
    private static readonly string[] Fields = ["id", "position", "text", "icon", "surfaces", "follow", "height", "maxDistance", "priority", "rgba", "mapLabel"];

    /// <summary>Reads a marker from JSON, for example
    /// <c>{"id":"boss_gate","position":{"x":3670,"y":820,"z":1500},"text":"Raid entrance","icon":"well_plant",
    /// "surfaces":["map","compass","world"],"follow":null,"height":2,"maxDistance":0,"priority":0,"rgba":4294967295,
    /// "mapLabel":"none"}</c>. Required: <c>id</c>, <c>position</c>, <c>text</c>. <c>icon</c> is a canonical key
    /// (<c>type:ffe5e444</c>) or an alias of the icon catalog, or null or absent for none; <c>surfaces</c> is a list of
    /// <c>map</c>, <c>compass</c>, <c>world</c> (default map and compass); <c>follow</c> must be null in files (a capture
    /// lease is not an entity id); <c>mapLabel</c> is <c>none</c> or <c>nativeTooltip</c>. Other fields default as in C#;
    /// an unknown field is an error. The values' own rules are checked by <see cref="Markers.Set"/>.</summary>
    /// <param name="json">One JSON object.</param>
    /// <returns>The marker.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="json"/> is null.</exception>
    /// <exception cref="ArgumentException">Not a marker object, a field of the wrong type or unknown, an icon name that
    /// is not a name, unknown or ambiguous.</exception>
    /// <exception cref="InvalidOperationException">An icon name before the icon catalog is ready
    /// (<c>Markers.Icons.IsReady</c>).</exception>
    /// <exception cref="KilnUnavailableException">An icon name while <c>markers.catalog</c> is off, or a <c>follow</c>
    /// lease while <c>markers.anchors.client</c> is off.</exception>
    [AnyThread]
    public static MarkerSpec FromJson(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(json);
        }
        catch (JsonException e)
        {
            throw new ArgumentException($"marker JSON: {e.Message}", nameof(json), e);
        }
        using (doc) return FromJson(doc.RootElement);
    }

    /// <summary>Reads a marker from a JSON element (one object of a larger document); the rules of
    /// <see cref="FromJson(string)"/>.</summary>
    /// <param name="json">A JSON object.</param>
    /// <returns>The marker.</returns>
    /// <exception cref="ArgumentException">See <see cref="FromJson(string)"/>.</exception>
    /// <exception cref="InvalidOperationException">See <see cref="FromJson(string)"/>.</exception>
    /// <exception cref="KilnUnavailableException">See <see cref="FromJson(string)"/>.</exception>
    [AnyThread]
    public static MarkerSpec FromJson(JsonElement json)
    {
        if (json.ValueKind != JsonValueKind.Object) throw Bad("", "must be an object");
        foreach (var p in json.EnumerateObject())
            if (Array.IndexOf(Fields, p.Name) < 0) throw Bad(p.Name, "is not a marker field (" + string.Join(", ", Fields) + ")");
        var id = Required(json, "id").ValueKind == JsonValueKind.String ? json.GetProperty("id").GetString()! : throw Bad("id", "must be a string");
        var text = Required(json, "text").ValueKind == JsonValueKind.String ? json.GetProperty("text").GetString()! : throw Bad("text", "must be a string");
        var position = ReadPosition(Required(json, "position"));
        var surfaces = json.TryGetProperty("surfaces", out var s) ? ReadSurfaces(s) : MarkerSurface.Map | MarkerSurface.Compass;
        var icon = json.TryGetProperty("icon", out var i) ? ReadIcon(i) : MarkerIcon.None;
        MarkerAnchor? follow = null;
        if (json.TryGetProperty("follow", out var f) && f.ValueKind != JsonValueKind.Null) follow = ReadFollow(f);
        var height = json.TryGetProperty("height", out var h) ? Single(h, "height") : 0f;
        var maxDistance = json.TryGetProperty("maxDistance", out var m) ? Single(m, "maxDistance") : 0f;
        var priority = 0;
        if (json.TryGetProperty("priority", out var pr) && (pr.ValueKind != JsonValueKind.Number || !pr.TryGetInt32(out priority)))
            throw Bad("priority", "must be a whole number");
        var rgba = 0xffffffffu;
        if (json.TryGetProperty("rgba", out var c) && (c.ValueKind != JsonValueKind.Number || !c.TryGetUInt32(out rgba)))
            throw Bad("rgba", "must be a whole number from 0 to 4294967295 (0xRRGGBBAA)");
        var label = MarkerMapLabel.None;
        if (json.TryGetProperty("mapLabel", out var l))
            label = l.ValueKind == JsonValueKind.String ? l.GetString() switch
            {
                "none" => MarkerMapLabel.None,
                "nativeTooltip" => MarkerMapLabel.NativeTooltip,
                _ => throw Bad("mapLabel", "must be \"none\" or \"nativeTooltip\""),
            } : throw Bad("mapLabel", "must be \"none\" or \"nativeTooltip\"");
        return new MarkerSpec(id, position, text, icon, surfaces, follow, height, maxDistance, priority, rgba, label);
    }

    /// <summary>This marker as one JSON object in the form <see cref="FromJson(string)"/> reads: every field written, the
    /// icon as its canonical key (or null). A marker that follows an entity is written with its lease
    /// (<c>{"anchor":"&lt;16 hex digits&gt;"}</c>), which is valid in this process and world only.</summary>
    /// <returns>The JSON text.</returns>
    [AnyThread]
    public string ToJson()
    {
        using var buffer = new MemoryStream();
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            w.WriteString("id", Id);
            w.WriteStartObject("position");
            w.WriteNumber("x", Position.X);
            w.WriteNumber("y", Position.Y);
            w.WriteNumber("z", Position.Z);
            w.WriteEndObject();
            w.WriteString("text", Text);
            if (Icon.Id == 0) w.WriteNull("icon");
            else w.WriteString("icon", "type:" + Icon.Id.ToString("x8", CultureInfo.InvariantCulture));
            w.WriteStartArray("surfaces");
            if ((Surfaces & MarkerSurface.Map) != 0) w.WriteStringValue("map");
            if ((Surfaces & MarkerSurface.Compass) != 0) w.WriteStringValue("compass");
            if ((Surfaces & MarkerSurface.World) != 0) w.WriteStringValue("world");
            w.WriteEndArray();
            if (Follow is null) w.WriteNull("follow");
            else
            {
                w.WriteStartObject("follow");
                w.WriteString("anchor", Follow.Handle.ToString("x16", CultureInfo.InvariantCulture));
                w.WriteEndObject();
            }
            w.WriteNumber("height", Height);
            w.WriteNumber("maxDistance", MaxDistance);
            w.WriteNumber("priority", Priority);
            w.WriteNumber("rgba", Rgba);
            w.WriteString("mapLabel", MapLabel == MarkerMapLabel.NativeTooltip ? "nativeTooltip" : "none");
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    // ---- fields ----

    private static ArgumentException Bad(string field, string what) =>
        new(field.Length == 0 ? $"marker JSON {what}" : $"marker JSON: '{field}' {what}");

    private static JsonElement Required(JsonElement o, string name) =>
        o.TryGetProperty(name, out var v) ? v : throw Bad(name, "is required");

    private static WorldPosition ReadPosition(JsonElement p)
    {
        if (p.ValueKind != JsonValueKind.Object) throw Bad("position", "must be an object {\"x\":..,\"y\":..,\"z\":..}");
        foreach (var q in p.EnumerateObject())
            if (q.Name is not ("x" or "y" or "z")) throw Bad("position", $"has an unknown field '{q.Name}'");
        double Axis(string n) => p.TryGetProperty(n, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out var d)
            ? d
            : throw Bad("position", $"needs a number '{n}'");
        return new WorldPosition(Axis("x"), Axis("y"), Axis("z"));
    }

    private static MarkerSurface ReadSurfaces(JsonElement s)
    {
        if (s.ValueKind != JsonValueKind.Array) throw Bad("surfaces", "must be a list of \"map\", \"compass\", \"world\"");
        MarkerSurface mask = 0;
        foreach (var e in s.EnumerateArray())
            mask |= e.ValueKind == JsonValueKind.String ? e.GetString() switch
            {
                "map" => MarkerSurface.Map,
                "compass" => MarkerSurface.Compass,
                "world" => MarkerSurface.World,
                _ => throw Bad("surfaces", $"has '{e.GetString()}': each must be \"map\", \"compass\" or \"world\""),
            } : throw Bad("surfaces", "must be a list of strings");
        if (mask == 0) throw Bad("surfaces", "must name at least one surface");
        return mask;
    }

    private static float Single(JsonElement v, string name) =>
        v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out var d) && double.IsFinite(d) && Math.Abs(d) <= float.MaxValue
            ? (float)d
            : throw Bad(name, "must be a finite number");

    private static MarkerIcon ReadIcon(JsonElement i)
    {
        if (i.ValueKind == JsonValueKind.Null) return MarkerIcon.None;
        if (i.ValueKind != JsonValueKind.String) throw Bad("icon", "must be an icon name (a canonical key or an alias) or null");
        return ResolveIcon(i.GetString()!);
    }

    private static MarkerAnchor? ReadFollow(JsonElement f)
    {
        if (f.ValueKind != JsonValueKind.Object || !f.TryGetProperty("anchor", out var a) || a.ValueKind != JsonValueKind.String)
            throw Bad("follow", "must be null or {\"anchor\":\"<16 hex digits>\"} (a capture lease of this process)");
        return FollowLease(a.GetString()!);
    }

    // An icon name through the catalog (the icon slice); until it is built the catalog's row is off.
    private static MarkerIcon ResolveIcon(string name)
    {
        MarkersCaps.Require(MarkersCaps.Catalog);
        throw new InvalidOperationException($"marker JSON: the icon '{name}' cannot be resolved: the icon catalog is not ready");
    }

    // A capture lease named in runtime JSON (the anchor slice keeps the registry of live leases); until it is built the
    // anchors' row is off.
    private static MarkerAnchor FollowLease(string handle)
    {
        MarkersCaps.Require(MarkersCaps.AnchorsClient);
        throw new ArgumentException($"marker JSON: 'follow' names the lease {handle}, which is not a live lease of this mod");
    }
}
