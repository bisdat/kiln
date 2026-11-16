// SPDX-License-Identifier: MIT
// Gem wave G1: what the gem read API and the gem events read from the game, behind one seam (IGemWorld) so the logic is
// tested with fakes. The live side reads kiln.items (catalog, item instances), kiln.gamedata (a gem's equipment.program
// and impactValues), kiln.world + kiln.types (Equipment.equipmentImpacts) and kiln.content (mods' items).
// docs/kiln/services/gems.md.
using System.Globalization;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>One impact slot of an entity's <c>Equipment.equipmentImpacts</c>: the item in it and its gem's impact.</summary>
internal readonly record struct GemImpactRow(int Slot, uint CarrierPide, ItemId CarrierItem, EntityId GemImpact, EntityId GemEntity);

/// <summary>One item entity as kiln.items reads it: its ref, the gem in its socket and the gem's level (none: no gem),
/// and the item's own level (a loose gem's level).</summary>
internal readonly record struct GemItemRead(ItemInstanceRef Item, ItemId Gem, int GemLevel, int Level);

/// <summary>What a gem's ItemInfo says about its effect (kiln.gamedata).</summary>
internal sealed record GemData(Guid Program, IReadOnlyDictionary<uint, GemConfigValue> Configs, bool IsClone);

/// <summary>What the gem API reads from the game. Tick thread.</summary>
internal interface IGemWorld
{
    /// <summary>The item catalog (kiln.items).</summary>
    IReadOnlyList<ItemInfo> Items();

    /// <summary>A key that changes when the catalog or game data may have changed.</summary>
    long CatalogKey();

    /// <summary>The program and configs of a gem's ItemInfo; null when kiln.gamedata does not know the item.</summary>
    GemData? Data(ItemInfo item);

    /// <summary>The item ids mods defined (kiln.content's new items).</summary>
    IReadOnlySet<uint> ModItems();

    /// <summary>The names mods gave their items, by item id (kiln.content's definitions: <c>displayName</c>, else the
    /// <c>"en"</c> or every-language text's name). The item catalog has display names for the game's items only.</summary>
    IReadOnlyDictionary<uint, string> ModNames() => new Dictionary<uint, string>();

    /// <summary>The 11 impact slots of an entity's Equipment; null when it has none.</summary>
    IReadOnlyList<GemImpactRow>? Impacts(EntityId holder);

    /// <summary>Item entities by raw id (null: not an item entity now), in the order given.</summary>
    IReadOnlyList<GemItemRead?> ReadEntities(IReadOnlyList<uint> pides);

    /// <summary>One item by ref; null when gone or stale.</summary>
    GemItemRead? Read(ItemInstanceRef item);

    /// <summary>Every item in a holder's inventories; null when it has none or it is not initialized yet.</summary>
    IReadOnlyList<GemItemRead>? ReadHolder(EntityId holder);

    /// <summary>The players to watch (entities with a SlotSelection: real players and spawned lab players).</summary>
    IReadOnlyList<EntityId> Players();

    /// <summary>The impact entity's parent (<c>ImpactHierarchy.parentImpactId</c>); none when it has none.</summary>
    EntityId ParentImpact(EntityId impact);
}

/// <summary>The live game behind <see cref="IGemWorld"/>.</summary>
internal sealed unsafe class LiveGemWorld : IGemWorld
{
    public IReadOnlyList<ItemInfo> Items() => Kiln.Items.All;

    public long CatalogKey()
    {
        var st = Content.Status();
        unchecked
        {
            return (Kiln.Items.All.Count * 1000003L) ^ ((long)(st?.Generation ?? 0) << 20) ^ (st?.Changed ?? 0);
        }
    }

    public GemData? Data(ItemInfo item)
    {
        if (!Content.GameDataAvailable) return null;
        var e = Content.ItemData.Get(item.Id.Value);
        if (e is null) return null;
        var program = Guid.Empty;
        try
        {
            program = GemJson.Program(e.GetJson("equipment.program"));
        }
        catch (KeyNotFoundException)
        {
        }
        // the raw config lists carry each config's id (the name map of GetJson("impactValues") keys them by alias,
        // and an alias can stand for several ids): lab 2026-11-16
        var configs = GemJson.Configs(Arrays(e, "impactValues.simple"), Arrays(e, "impactValues.scaled"), Vocabulary.AliasOf);
        return new GemData(program, configs, e.IsClone);
    }

    private static JsonNode? Arrays(GameDataEntry e, string path)
    {
        try
        {
            return e.GetJson(path);
        }
        catch (KeyNotFoundException)
        {
            return null;  // the list is empty or absent
        }
    }

    public IReadOnlySet<uint> ModItems()
    {
        if (!NewContent.Available) return new HashSet<uint>();
        return NewContent.Definitions().Where(d => d.Kind == ContentKind.Items).Select(d => d.Id).ToHashSet();
    }

    public IReadOnlyDictionary<uint, string> ModNames()
    {
        var names = new Dictionary<uint, string>();
        if (!NewContent.Available) return names;
        foreach (var d in NewContent.Definitions())
            if (d.Kind == ContentKind.Items && GemJson.DisplayName(d.DefinitionJson) is { } n) names[d.Id] = n;
        return names;
    }

    public IReadOnlyList<GemImpactRow>? Impacts(EntityId holder)
    {
        var layout = EquipmentLayout.Get();
        var bytes = Kiln.World.ReadBytes(holder, "Equipment");
        return bytes is null ? null : layout.Parse(bytes);
    }

    public IReadOnlyList<GemItemRead?> ReadEntities(IReadOnlyList<uint> pides)
    {
        ItemInstances.Require();
        var result = new GemItemRead?[pides.Count];
        if (pides.Count == 0) return result;
        var buf = new ItemsNative.Instance[pides.Count];
        for (var i = 0; i < pides.Count; i++) buf[i].Entity = ItemRefs.Stampable(pides[i]) ? pides[i] : 0;
        ItemInstances.ReadRaw(buf, 0, "Gems");
        for (var i = 0; i < pides.Count; i++)
            if (buf[i].Entity != 0 && (buf[i].Flags & ItemsNative.InstFound) != 0)
                result[i] = Of(ItemInstances.InfoOf(in buf[i]));
        return result;
    }

    public GemItemRead? Read(ItemInstanceRef item) => ItemInstances.Read(item) is { } info ? Of(info) : null;

    public IReadOnlyList<GemItemRead>? ReadHolder(EntityId holder)
    {
        var list = ItemInstances.ReadHolder(holder, out var initialized);
        return initialized ? list.Select(Of).ToList() : null;
    }

    public IReadOnlyList<EntityId> Players() => Kiln.World.Find("SlotSelection");

    public EntityId ParentImpact(EntityId impact)
    {
        var b = Kiln.World.ReadBytes(impact, "ImpactHierarchy");
        var off = HierarchyParentOffset.Value;
        return b is null || off < 0 || b.Length < off + 4 ? EntityId.None : new EntityId(BitConverter.ToUInt32(b, off));
    }

    private static readonly Lazy<int> HierarchyParentOffset = new(() =>
        GameTypes.Find("ImpactHierarchy")?.Field("parentImpactId") is { } f ? f.Offset : -1);

    private static GemItemRead Of(ItemInstanceInfo i) => new(i.Item, i.Record.Gem, i.Record.Gem.IsNone ? 0 : i.Record.GemLevel, i.Record.Level);

    /// <summary>The config vocabulary of kiln.gamedata (config id -> its alias), read again every 2 s at most.</summary>
    private static class Vocabulary
    {
        private static Dictionary<uint, string> _alias = new();
        private static long? _readAt;

        public static string? AliasOf(uint id)
        {
            var now = Environment.TickCount64;
            if (_readAt is not { } at || now - at > 2000)
            {
                _readAt = now;
                _alias = GemJson.AliasIndex(Content.ConfigVocabulary());
            }
            return _alias.GetValueOrDefault(id);
        }
    }
}

/// <summary>Where <c>Equipment.equipmentImpacts</c> keeps what the gem API reads, from the game's reflection.</summary>
internal sealed record EquipmentLayout(int ImpactsOffset, int Stride, int Count, int PideOffset, int ItemIdOffset,
    int GemImpactOffset, int GemEntityOffset)
{
    /// <summary>The dedicated server's layout on build 26430253 (sigdb/types): the tests' fixture.</summary>
    internal static readonly EquipmentLayout Server26430253 = new(8, 72, 11, 12, 8, 64, 68);

    private static EquipmentLayout? _live;

    /// <summary>The running build's layout; throws <see cref="KilnUnavailableException"/>(<c>gems.read</c>) when the
    /// game's reflection does not have it.</summary>
    internal static EquipmentLayout Get() => Live() ?? throw new KilnUnavailableException(GemsCaps.Read,
        "the game's reflection has no Equipment.equipmentImpacts[].weaponGemImpactEntityId on this build");

    /// <summary>The running build's layout once found (kept for the process: the types do not change), else null.</summary>
    internal static EquipmentLayout? Live() => _live ??= FromTypes();

    /// <summary>The layout from kiln.types, or null when a field is missing. Pure apart from the type table.</summary>
    internal static EquipmentLayout? FromTypes()
    {
        if (!GameTypes.Available) return null;
        var eq = GameTypes.Find("Equipment");
        if (eq?.Field("equipmentImpacts") is not { } impacts) return null;
        var arr = impacts.Type.Resolved;
        if (arr.Kind != GameTypeKind.StaticArray || arr.Inner is not { } el || arr.ArrayLength <= 0) return null;
        el = el.Resolved;
        if (el.Field("itemInstance") is not { } inst || inst.Type.Resolved.Field("pide") is not { } pide ||
            inst.Type.Resolved.Field("itemId") is not { } item || el.Field("weaponGemImpactEntityId") is not { } gi ||
            el.Field("weaponGemEntityId") is not { } ge) return null;
        return new EquipmentLayout(impacts.Offset, el.Size, arr.ArrayLength, inst.Offset + pide.Offset, inst.Offset + item.Offset,
            gi.Offset, ge.Offset);
    }

    /// <summary>The slots of one Equipment component's bytes (a slot past the bytes is left out). Pure.</summary>
    internal IReadOnlyList<GemImpactRow> Parse(byte[] bytes)
    {
        var rows = new List<GemImpactRow>(Count);
        for (var i = 0; i < Count; i++)
        {
            var b = ImpactsOffset + i * Stride;
            if (b + Stride > bytes.Length) break;
            rows.Add(new GemImpactRow(i, U(bytes, b + PideOffset), new ItemId(U(bytes, b + ItemIdOffset)),
                new EntityId(U(bytes, b + GemImpactOffset)), new EntityId(U(bytes, b + GemEntityOffset))));
        }
        return rows;
    }

    private static uint U(byte[] b, int at) => BitConverter.ToUInt32(b, at);
}

/// <summary>kiln.gamedata's JSON for a gem's ItemInfo (<c>GetJson</c>), read into the gem API's types. Pure.</summary>
internal static class GemJson
{
    /// <summary>The program GUID of <c>GetJson("equipment.program")</c>: <c>{"value": "&lt;guid&gt;" | "none"}</c>
    /// (also a bare value, or the GUID one level down). Empty when none.</summary>
    internal static Guid Program(JsonNode? node)
    {
        var v = node is JsonObject o && o.ContainsKey("value") ? o["value"] : node;
        if (v is JsonObject inner)
            foreach (var (_, x) in inner)
                if (Guid.TryParse(Text(x), out var g)) return g;
        return Guid.TryParse(Text(v), out var guid) ? guid : Guid.Empty;
    }

    /// <summary>The configs of a gem from <c>GetJson("impactValues.simple")</c> and <c>GetJson("impactValues.scaled")</c>:
    /// <c>{"value": [{"$type": kind, "configId": id, "value": v, "scaleFactor": s (scaled only), ...}]}</c> (also a bare
    /// array). The alias is the vocabulary's (<paramref name="aliasOf"/>), the name kiln.gamedata writes for the id.
    /// An element without a config id is left out.</summary>
    internal static IReadOnlyDictionary<uint, GemConfigValue> Configs(JsonNode? simple, JsonNode? scaled, Func<uint, string?> aliasOf)
    {
        var result = new Dictionary<uint, GemConfigValue>();
        foreach (var (list, isScaled) in new[] { (simple, false), (scaled, true) })
        {
            var arr = list is JsonObject o ? o["value"] as JsonArray : list as JsonArray;
            if (arr is null) continue;
            foreach (var el in arr)
            {
                if (el is not JsonObject c || Number(c["configId"]) is not { } idNumber || idNumber is < 1 or > uint.MaxValue) continue;
                var id = (uint)idNumber;
                var raw = c["value"];
                var number = Number(raw);
                var scale = isScaled ? Number(c["scaleFactor"]) : null;
                result[id] = new GemConfigValue(id, aliasOf(id), Text(c["$type"]) ?? "", number ?? 0, scale)
                {
                    Text = number is null ? Text(raw) : null,
                };
            }
        }
        return result;
    }

    /// <summary>The name a kiln.content item definition gives (its JSON as <c>NewContent.Definitions()</c> lists it):
    /// <c>displayName</c>, else <c>texts.en.name</c>, else <c>texts["*"].name</c>; null without one.</summary>
    internal static string? DisplayName(string? definitionJson)
    {
        if (string.IsNullOrEmpty(definitionJson)) return null;
        JsonNode? d;
        try
        {
            d = JsonNode.Parse(definitionJson);
        }
        catch (System.Text.Json.JsonException)
        {
            return null;
        }
        if (d is not JsonObject o) return null;
        foreach (var n in new[] { o["displayName"], o["texts"]?["en"]?["name"], o["texts"]?["*"]?["name"] })
            if (Text(n) is { Length: > 0 } s) return s;
        return null;
    }

    /// <summary>id -> alias from <c>Content.ConfigVocabulary()</c>: <c>[{"id": "0x..", "names": [..], "alias": ".."}]</c>
    /// (a row without an alias has none).</summary>
    internal static Dictionary<uint, string> AliasIndex(string json)
    {
        var d = new Dictionary<uint, string>();
        JsonNode? root;
        try
        {
            root = JsonNode.Parse(json);
        }
        catch (System.Text.Json.JsonException)
        {
            return d;
        }
        if (root is not JsonArray rows) return d;
        foreach (var r in rows)
        {
            if (Text(r?["alias"]) is not { Length: > 0 } alias || Text(r?["id"]) is not { } idText) continue;
            if (idText.StartsWith("0x", StringComparison.OrdinalIgnoreCase) &&
                uint.TryParse(idText.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var id))
                d[id] = alias;
        }
        return d;
    }

    private static double? Number(JsonNode? n)
    {
        if (n is not JsonValue v) return null;
        if (v.TryGetValue<double>(out var d)) return d;
        if (v.TryGetValue<long>(out var l)) return l;
        if (v.TryGetValue<ulong>(out var u)) return u;
        if (v.TryGetValue<bool>(out var b)) return b ? 1 : 0;
        return null;
    }

    private static string? Text(JsonNode? n) => n is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;
}
