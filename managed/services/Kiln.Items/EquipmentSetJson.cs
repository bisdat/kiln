// SPDX-License-Identifier: MIT
// The session JSON of an equipment set (chapter 10 §5, schema 1): a round trip for the current server-world session and
// for debugging, never a saved loadout. The scope is written and read back as it was; applying a set read from an older
// session comes back StaleSnapshot (nothing is re-resolved after a restart or a rejoin).
//
// { "schema": 1,
//   "scope": { "processSession": "<guid>", "worldEpoch": 7,
//              "player": { "entity": 1, "hasSpawnTime": true, "spawnTime": 123, "lifeSerial": 9, "playerHandle": 4, "joinedStep": 20 } },
//   "entries": [ { "slot": "ring0", "selection": { "kind": "instance", "item": { "entity": 2552, "serial": 1152921504606846977, "item": 2034509824 } } },
//                { "slot": "arrow", "selection": { "kind": "stack", "item": 1234, "source": { "inventory": 2544, "index": 3 }, "expectedCount": 40 } },
//                { "slot": "ring1", "selection": { "kind": "empty" } } ] }
//
// 64-bit values are JSON integers, written and read exactly. An item ref is { entity, serial, item } (EntityRef.Serial as
// it is, the item type as its number); a slot id is { inventory, index }; a slot name is the EquipmentSlot name with a
// lower-case first letter (read in any case).
using System.Text;
using System.Text.Json;

namespace Kiln;

/// <summary>Writes and reads an <see cref="EquipmentSet"/> as JSON (schema 1) for the current session or for a log.
/// Not a saved loadout: a set from another process, server world or life of the player is refused when it is applied
/// (<see cref="EquipmentSetStatus.StaleSnapshot"/>), and nothing in it is re-resolved. Since 0.2.0-dev.29.</summary>
public static class EquipmentSetJson
{
    /// <summary>The schema this Kiln writes and reads.</summary>
    public const int Schema = 1;

    /// <summary>The set as JSON.</summary>
    /// <exception cref="ArgumentException">The set breaks the rules of a set (see <c>Items.EquipSet</c>).</exception>
    [AnyThread]
    public static string Serialize(EquipmentSet set, bool indented = false)
    {
        var entries = EquipmentSetRules.Check(set, nameof(set));
        using var buffer = new MemoryStream();
        using (var w = new Utf8JsonWriter(buffer, new JsonWriterOptions { Indented = indented }))
        {
            w.WriteStartObject();
            w.WriteNumber("schema", Schema);
            w.WriteStartObject("scope");
            w.WriteString("processSession", set.Scope.ProcessSession.ToString("D"));
            w.WriteNumber("worldEpoch", set.Scope.WorldEpoch);
            var p = set.Scope.Player;
            w.WriteStartObject("player");
            w.WriteNumber("entity", p.Entity);
            w.WriteBoolean("hasSpawnTime", p.HasSpawnTime);
            w.WriteNumber("spawnTime", p.SpawnTime);
            w.WriteNumber("lifeSerial", p.LifeSerial);
            w.WriteNumber("playerHandle", p.PlayerHandle);
            w.WriteNumber("joinedStep", p.JoinedStep);
            w.WriteEndObject();
            w.WriteEndObject();
            w.WriteStartArray("entries");
            foreach (var e in entries)
            {
                w.WriteStartObject();
                w.WriteString("slot", SlotName(e.Slot));
                w.WriteStartObject("selection");
                switch (e.Selection)
                {
                    case EmptyEquipment:
                        w.WriteString("kind", "empty");
                        break;
                    case InstanceEquipment i:
                        w.WriteString("kind", "instance");
                        w.WriteStartObject("item");
                        w.WriteNumber("entity", i.Item.Entity.Id.Value);
                        w.WriteNumber("serial", i.Item.Entity.Serial);
                        w.WriteNumber("item", i.Item.Item.Value);
                        w.WriteEndObject();
                        break;
                    case StackEquipment s:
                        w.WriteString("kind", "stack");
                        w.WriteNumber("item", s.Item.Value);
                        w.WriteStartObject("source");
                        w.WriteNumber("inventory", s.Source.Inventory.Value);
                        w.WriteNumber("index", s.Source.Index);
                        w.WriteEndObject();
                        w.WriteNumber("expectedCount", s.ExpectedCount);
                        break;
                }
                w.WriteEndObject();
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    /// <summary>A set from JSON written by <see cref="Serialize"/> (schema 1). The scope is taken as written: a set of an
    /// earlier session is read, and refused when it is applied.</summary>
    /// <exception cref="ArgumentException">Not JSON, another schema, a missing or malformed field, an unknown slot or
    /// selection kind, or a set that breaks the rules of a set.</exception>
    [AnyThread]
    public static EquipmentSet Deserialize(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(json);
        }
        catch (JsonException e)
        {
            throw new ArgumentException($"not JSON: {e.Message}", nameof(json), e);
        }
        using (doc)
        {
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) throw Bad("$", "an object");
            var schema = Int(root, "schema", "$");
            if (schema != Schema) throw new ArgumentException($"schema {schema} is not one this Kiln reads (it reads {Schema})", nameof(json));
            var scopeEl = Obj(root, "scope", "$");
            var playerEl = Obj(scopeEl, "player", "$.scope");
            if (!Guid.TryParse(Str(scopeEl, "processSession", "$.scope"), out var session)) throw Bad("$.scope.processSession", "a GUID");
            var player = new EventEntityKey(
                U32(playerEl, "entity", "$.scope.player"),
                Bool(playerEl, "hasSpawnTime", "$.scope.player"),
                U64(playerEl, "spawnTime", "$.scope.player"),
                U64(playerEl, "lifeSerial", "$.scope.player"),
                U64(playerEl, "playerHandle", "$.scope.player"),
                U64(playerEl, "joinedStep", "$.scope.player"));
            var scope = new EquipmentSetScope(session, U64(scopeEl, "worldEpoch", "$.scope"), player);
            if (!root.TryGetProperty("entries", out var list) || list.ValueKind != JsonValueKind.Array) throw Bad("$.entries", "an array");
            var entries = new List<EquipmentSetEntry>();
            var n = 0;
            foreach (var el in list.EnumerateArray())
            {
                var at = $"$.entries[{n++}]";
                if (el.ValueKind != JsonValueKind.Object) throw Bad(at, "an object");
                var slotName = Str(el, "slot", at);
                if (!TryParseSlot(slotName, out var slot)) throw new ArgumentException($"{at}.slot: '{slotName}' is not an equipment slot", nameof(json));
                var sel = Obj(el, "selection", at);
                var kind = Str(sel, "kind", $"{at}.selection");
                EquipmentSetSelection selection = kind switch
                {
                    "empty" => new EmptyEquipment(),
                    "instance" => new InstanceEquipment(Instance(Obj(sel, "item", $"{at}.selection"), $"{at}.selection.item")),
                    "stack" => new StackEquipment(new ItemId(U32(sel, "item", $"{at}.selection")),
                        Slot(Obj(sel, "source", $"{at}.selection"), $"{at}.selection.source"), Int(sel, "expectedCount", $"{at}.selection")),
                    _ => throw new ArgumentException($"{at}.selection.kind: '{kind}' is not empty, instance or stack", nameof(json)),
                };
                entries.Add(new EquipmentSetEntry(slot, selection));
            }
            var set = new EquipmentSet(scope, entries);
            EquipmentSetRules.Check(set, nameof(json));
            return set;
        }
    }

    // ---- names ----

    internal static string SlotName(EquipmentSlot slot)
    {
        var s = slot.ToString();
        return char.ToLowerInvariant(s[0]) + s[1..];
    }

    internal static bool TryParseSlot(string name, out EquipmentSlot slot)
    {
        slot = default;
        if (name.Length == 0 || !char.IsLetter(name[0])) return false;  // a number is not a slot name
        return Enum.TryParse(name, ignoreCase: true, out slot) && Enum.IsDefined(slot);
    }

    // ---- readers ----

    private static ItemInstanceRef Instance(JsonElement el, string at) =>
        new(new EntityRef(new EntityId(U32(el, "entity", at)), U64(el, "serial", at)), new ItemId(U32(el, "item", at)));

    private static SlotId Slot(JsonElement el, string at)
    {
        var index = Int(el, "index", at);
        if (index is < 0 or > 7) throw Bad($"{at}.index", "0 to 7");
        return SlotId.Of(new EntityId(U32(el, "inventory", at)), index);
    }

    private static JsonElement Field(JsonElement o, string name, string at) =>
        o.TryGetProperty(name, out var v) ? v : throw new ArgumentException($"{at}.{name} is missing", "json");

    private static JsonElement Obj(JsonElement o, string name, string at)
    {
        var v = Field(o, name, at);
        return v.ValueKind == JsonValueKind.Object ? v : throw Bad($"{at}.{name}", "an object");
    }

    private static string Str(JsonElement o, string name, string at)
    {
        var v = Field(o, name, at);
        return v.ValueKind == JsonValueKind.String ? v.GetString()! : throw Bad($"{at}.{name}", "a string");
    }

    private static bool Bool(JsonElement o, string name, string at)
    {
        var v = Field(o, name, at);
        return v.ValueKind is JsonValueKind.True or JsonValueKind.False ? v.GetBoolean() : throw Bad($"{at}.{name}", "true or false");
    }

    private static ulong U64(JsonElement o, string name, string at)
    {
        var v = Field(o, name, at);
        return v.ValueKind == JsonValueKind.Number && v.TryGetUInt64(out var x) ? x : throw Bad($"{at}.{name}", "an integer of 0 to 2^64-1");
    }

    private static uint U32(JsonElement o, string name, string at)
    {
        var v = Field(o, name, at);
        return v.ValueKind == JsonValueKind.Number && v.TryGetUInt32(out var x) ? x : throw Bad($"{at}.{name}", "an integer of 0 to 2^32-1");
    }

    private static int Int(JsonElement o, string name, string at)
    {
        var v = Field(o, name, at);
        return v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var x) ? x : throw Bad($"{at}.{name}", "a 32-bit integer");
    }

    private static ArgumentException Bad(string at, string what) => new($"{at} must be {what}", "json");
}
