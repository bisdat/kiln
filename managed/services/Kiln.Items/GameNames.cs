// SPDX-License-Identifier: MIT
// The game's own names for players, read from its resident data at run time: an item's, a workshop's and a recipe's, in the
// game's text language. No world is needed, so they also work in the main menu and on a client that joined another's world
// (where kiln.items has no catalog). Since 0.2.0-dev.29.15.
//
// Where they come from (docs/re/game-ui.md "Names and icons for mod UIs"; layouts from the game's reflection, kiln.types):
//  - keen::LocaTagCollectionResource (one resource, game clients only): `languages`, a BlobArray of
//    {LanguageId language, ResourceContentReference data}; each language's content blob is a BlobArray of keen::LocaTagResource
//    {LocaTagId id, BlobString text, ...}. A dedicated server's data has neither (docs/re/world-status.md section 12).
//  - keen::ItemInfo `name`, keen::RecipeInfo `recipeName`: a NameLocaTag (a 16-byte GUID); its tag id is FNV-1a-32 of the bytes.
//  - keen::WorkshopRegistryResource `workshops` (WorkshopResource {workshopId, name: LocaTagId, item}), keen::RecipeRegistryResource
//    `recipes` (RecipeInfo {recipeId, output: BlobArray of {itemRef, item, count}, recipeName}): on both sides.
//  - The game's items by id: every keen::ItemInfo resource of the game's data (not a mod's new one), id = FNV-1a-32 of its GUID.
// Reads go through kiln.assets (resource_read, blob_read). Nothing here waits for them on the caller's thread: the first
// question starts a background read and is answered null (the callers fall back) until it is done.
using System.Buffers.Binary;
using System.Text;
using System.Text.RegularExpressions;

namespace Kiln;

/// <summary>The reflected offsets <see cref="GameNames"/> reads with (the game's own; <see cref="Measured"/> is client 26430625's).</summary>
internal sealed record GameNameLayout(
    int Languages, int LanguageStride, int LanguageValue, int LanguageData,
    int TagStride, int TagId, int TagText,
    int ItemName,
    int Workshops, int WorkshopStride, int WorkshopId, int WorkshopName, int WorkshopItem,
    int Recipes, int RecipeStride, int RecipeId, int RecipeOutput, int RecipeName, int OutputStride, int OutputItem)
{
    /// <summary>Client 26430625 and server 26430253 (tools/kfc types): what the tests use.</summary>
    public static readonly GameNameLayout Measured = new(0x20, 40, 0, 8, 24, 0, 4, 0x48, 8, 56, 0x14, 0x18, 0x1c, 0, 168, 0x10, 0x30, 0x68, 24, 0x10);

    /// <summary>The running game's offsets from kiln.types, or null when one of the types or fields is missing.</summary>
    public static GameNameLayout? Reflected()
    {
        static int F(string type, string field) => GameTypes.Find(type)?.Field(field)?.Offset ?? -1;
        static int S(string type) => GameTypes.Find(type)?.Size ?? -1;
        if (!GameTypes.Available) return null;
        var l = new GameNameLayout(
            F("keen::LocaTagCollectionResource", "languages"), S("keen::LocaTagCollectionResourceLanguage"),
            F("keen::LocaTagCollectionResourceLanguage", "language"), F("keen::LocaTagCollectionResourceLanguage", "data"),
            S("keen::LocaTagResource"), F("keen::LocaTagResource", "id"), F("keen::LocaTagResource", "text"),
            F("keen::ItemInfo", "name"),
            F("keen::WorkshopRegistryResource", "workshops"), S("keen::WorkshopResource"), F("keen::WorkshopResource", "workshopId"),
            F("keen::WorkshopResource", "name"), F("keen::WorkshopResource", "item"),
            F("keen::RecipeRegistryResource", "recipes"), S("keen::RecipeInfo"), F("keen::RecipeInfo", "recipeId"),
            F("keen::RecipeInfo", "output"), F("keen::RecipeInfo", "recipeName"), S("keen::RecipeItemStackResource"),
            F("keen::RecipeItemStackResource", "item"));
        return l.Valid ? l : null;
    }

    public bool Valid => new[] { Languages, LanguageValue, LanguageData, TagId, TagText, ItemName, Workshops, WorkshopId, WorkshopName,
        WorkshopItem, Recipes, RecipeId, RecipeOutput, RecipeName, OutputItem }.All(x => x >= 0) &&
        LanguageStride > 0 && TagStride > 0 && WorkshopStride > 0 && RecipeStride > 0 && OutputStride > 0;
}

/// <summary>Pure readers of the game's name data (the bytes as kiln.assets gives them). Host-tested.</summary>
internal static class GameNameData
{
    private static readonly Regex Markup = new("<[^<>]{1,64}>", RegexOptions.Compiled | RegexOptions.CultureInvariant);

    /// <summary>A keen::BlobArray {i32 offset from the field, u32 count} at <paramref name="at"/>: where its elements start
    /// and how many there are; false when it does not fit the data.</summary>
    public static bool BlobArray(ReadOnlySpan<byte> d, int at, int stride, out int start, out int count)
    {
        start = count = 0;
        if (at < 0 || at > d.Length - 8) return false;
        var rel = BinaryPrimitives.ReadInt32LittleEndian(d[at..]);
        var n = BinaryPrimitives.ReadUInt32LittleEndian(d[(at + 4)..]);
        var s = (long)at + rel;
        if (n == 0) return true;
        if (s < 0 || n > int.MaxValue / Math.Max(stride, 1) || s + (long)n * stride > d.Length) return false;
        start = (int)s;
        count = (int)n;
        return true;
    }

    /// <summary>A keen::BlobString {i32 offset from the field, u32 length} at <paramref name="at"/>, or null.</summary>
    public static string? BlobString(ReadOnlySpan<byte> d, int at)
    {
        if (at < 0 || at > d.Length - 8) return null;
        var rel = BinaryPrimitives.ReadInt32LittleEndian(d[at..]);
        var n = BinaryPrimitives.ReadUInt32LittleEndian(d[(at + 4)..]);
        var s = (long)at + rel;
        if (n == 0) return "";
        if (s < 0 || n > 1 << 16 || s + n > d.Length) return null;
        return Encoding.UTF8.GetString(d.Slice((int)s, (int)n));
    }

    /// <summary>FNV-1a-32 over 16 bytes: the tag id of a NameLocaTag, the id of an item's GUID. 0 for no reference (all zero).</summary>
    public static uint Fnv16(ReadOnlySpan<byte> g)
    {
        if (g.Length < 16) return 0;
        var zero = true;
        var h = 0x811c9dc5u;
        for (var i = 0; i < 16; i++)
        {
            zero &= g[i] == 0;
            h = (h ^ g[i]) * 0x01000193u;
        }
        return zero ? 0 : h;
    }

    /// <summary>The content hash (16 bytes) of <paramref name="languageValue"/>'s table in a LocaTagCollectionResource, or null.</summary>
    public static byte[]? LanguageBlob(ReadOnlySpan<byte> collection, GameNameLayout l, long languageValue)
    {
        if (!BlobArray(collection, l.Languages, l.LanguageStride, out var start, out var n)) return null;
        for (var i = 0; i < n; i++)
        {
            var e = start + i * l.LanguageStride;
            if (e + l.LanguageData + 16 > collection.Length || e + l.LanguageValue + 4 > collection.Length) return null;
            if (BinaryPrimitives.ReadInt32LittleEndian(collection[(e + l.LanguageValue)..]) == languageValue)
                return collection.Slice(e + l.LanguageData, 16).ToArray();
        }
        return null;
    }

    /// <summary>One language's texts: tag id -> text, markup tags (<c>&lt;b&gt;</c>) taken out. Null when the blob does not
    /// read as a table. At most 2,000,000 texts.</summary>
    public static Dictionary<uint, string>? Table(ReadOnlySpan<byte> blob, GameNameLayout l)
    {
        if (!BlobArray(blob, 0, l.TagStride, out var start, out var n) || n > 2_000_000) return null;
        var t = new Dictionary<uint, string>(n);
        for (var i = 0; i < n; i++)
        {
            var r = start + i * l.TagStride;
            if (r + l.TagId + 4 > blob.Length) return null;
            var id = BinaryPrimitives.ReadUInt32LittleEndian(blob[(r + l.TagId)..]);
            var s = BlobString(blob, r + l.TagText);
            if (s is null) return null;
            if (id != 0) t[id] = Plain(s);
        }
        return t;
    }

    public static string Plain(string s) => s.IndexOf('<') < 0 ? s.Trim() : Markup.Replace(s, "").Trim();

    /// <summary>An ItemInfo's name tag id (FNV-1a of its NameLocaTag), 0 when it has none.</summary>
    public static uint ItemNameTag(ReadOnlySpan<byte> itemInfo, GameNameLayout l) =>
        l.ItemName + 16 <= itemInfo.Length ? Fnv16(itemInfo.Slice(l.ItemName, 16)) : 0;

    /// <summary>The workshops of a WorkshopRegistryResource: id -> (name tag, the item that places it).</summary>
    public static Dictionary<uint, (uint NameTag, uint Item)> Workshops(ReadOnlySpan<byte> reg, GameNameLayout l)
    {
        var map = new Dictionary<uint, (uint, uint)>();
        if (!BlobArray(reg, l.Workshops, l.WorkshopStride, out var start, out var n)) return map;
        for (var i = 0; i < n; i++)
        {
            var w = start + i * l.WorkshopStride;
            if (w + Math.Max(l.WorkshopItem, Math.Max(l.WorkshopId, l.WorkshopName)) + 4 > reg.Length) break;
            var id = BinaryPrimitives.ReadUInt32LittleEndian(reg[(w + l.WorkshopId)..]);
            if (id != 0)
                map[id] = (BinaryPrimitives.ReadUInt32LittleEndian(reg[(w + l.WorkshopName)..]), BinaryPrimitives.ReadUInt32LittleEndian(reg[(w + l.WorkshopItem)..]));
        }
        return map;
    }

    /// <summary>The recipes of a RecipeRegistryResource: id -> (recipeName tag, first output's item; 0 when none).</summary>
    public static Dictionary<uint, (uint NameTag, uint FirstOutput)> Recipes(ReadOnlySpan<byte> reg, GameNameLayout l)
    {
        var map = new Dictionary<uint, (uint, uint)>();
        if (!BlobArray(reg, l.Recipes, l.RecipeStride, out var start, out var n)) return map;
        for (var i = 0; i < n; i++)
        {
            var r = start + i * l.RecipeStride;
            if (r + l.RecipeStride > reg.Length) break;
            var id = BinaryPrimitives.ReadUInt32LittleEndian(reg[(r + l.RecipeId)..]);
            var tag = Fnv16(reg.Slice(r + l.RecipeName, 16));
            uint first = 0;
            if (BlobArray(reg, r + l.RecipeOutput, l.OutputStride, out var os, out var on) && on > 0 && os + l.OutputItem + 4 <= reg.Length)
                first = BinaryPrimitives.ReadUInt32LittleEndian(reg[(os + l.OutputItem)..]);
            if (id != 0) map[id] = (tag, first);
        }
        return map;
    }

    /// <summary>"De_De" (a keen::LanguageId value's name) -> "de-de".</summary>
    public static string LanguageKey(string enumName) => enumName.Replace('_', '-').ToLowerInvariant();
}

/// <summary>The game's names in the game's text language, from its own data (see the file header). Any thread; never waits
/// for a read: an answer that is not loaded yet is null and starts the load in the background.</summary>
internal static class GameNames
{
    private const uint ItemInfoType = 0xb5ce8765;

    private sealed class Loaded
    {
        public required GameNameLayout Layout;
        public required Dictionary<uint, Guid> Items;                              // the game's item ids -> GUIDs
        public required Dictionary<uint, (uint NameTag, uint Item)> Workshops;
        public required Dictionary<uint, (uint NameTag, uint FirstOutput)> Recipes;
        public required byte[]? Collection;                                      // the LocaTagCollectionResource (clients)
        public required Dictionary<string, long> LanguageValues;                 // "de-de" -> keen::LanguageId
    }

    private static readonly object Gate = new();
    private static Loaded? _data;
    private static Task? _loading;
    private static long _failedAt;
    private static int _failures;
    private static (string Language, Dictionary<uint, string>? Texts)? _texts;
    private static Task? _textsLoading;
    private static (string Language, long At, int Failures) _textsFailed;  // a language whose table could not be read yet
    private static readonly Dictionary<uint, uint> ItemTags = new();

    /// <summary>Tests: the data to answer from (null: the game's again).</summary>
    internal static Func<string>? LanguageOverride;

    internal static void UseForTests(GameNameLayout layout, Dictionary<uint, Guid> items, Dictionary<uint, (uint, uint)> workshops,
        Dictionary<uint, (uint, uint)> recipes, string language, Dictionary<uint, string>? texts, Dictionary<uint, uint>? itemTags = null)
    {
        lock (Gate)
        {
            _data = new Loaded { Layout = layout, Items = items, Workshops = workshops, Recipes = recipes, Collection = null, LanguageValues = new() };
            _texts = (language, texts);
            _loading = _textsLoading = null;
            ItemTags.Clear();
            if (itemTags != null) foreach (var (k, v) in itemTags) ItemTags[k] = v;
            LanguageOverride = () => language;
        }
    }

    internal static void ResetForTests()
    {
        lock (Gate)
        {
            _data = null;
            _texts = null;
            _loading = _textsLoading = null;
            _failedAt = 0;
            _failures = 0;
            _textsFailed = default;
            ItemTags.Clear();
            LanguageOverride = null;
        }
    }

    /// <summary>The language the texts are wanted in: the game's ("de-de") on a client, "en-us" on a server.</summary>
    public static string Language => LanguageOverride?.Invoke() ?? GameLanguageSource.Current();

    /// <summary>The game's data, or null while it loads (started here) or when it cannot be read (tried again after 30 s,
    /// then after twice as long each time, at most 10 minutes).</summary>
    private static Loaded? Data(bool wait = false)
    {
        Task? task;
        lock (Gate)
        {
            if (_data != null) return _data;
            if (_loading == null)
            {
                if (_failedAt != 0 && Environment.TickCount64 - _failedAt < RetryDelay.Of(_failures, 30_000, 600_000)) return null;
                _loading = Task.Run(LoadData);
            }
            task = _loading;
        }
        if (wait) task.Wait(TimeSpan.FromSeconds(10));
        lock (Gate) return _data;
    }

    private static void LoadData()
    {
        Loaded? d = null;
        try
        {
            if (ResourceStore.Available && GameNameLayout.Reflected() is { } layout)
            {
                var items = new Dictionary<uint, Guid>();
                Span<byte> g = stackalloc byte[16];
                foreach (var id in ResourceStore.List(ItemInfoType))
                    if (id.Part == 0 && !ResourceStore.IsNew(id))
                    {
                        id.Guid.TryWriteBytes(g);
                        items.TryAdd(GameNameData.Fnv16(g), id.Guid);
                    }
                d = new Loaded
                {
                    Layout = layout,
                    Items = items,
                    Workshops = One("keen::WorkshopRegistryResource") is { } w ? GameNameData.Workshops(w, layout) : new(),
                    Recipes = One("keen::RecipeRegistryResource") is { } r ? GameNameData.Recipes(r, layout) : new(),
                    Collection = One("keen::LocaTagCollectionResource"),
                    LanguageValues = (GameTypes.Find("keen::LanguageId")?.EnumValues ?? [])
                        .GroupBy(v => GameNameData.LanguageKey(v.Name)).ToDictionary(g => g.Key, g => g.First().Value),
                };
                if (d.Items.Count == 0) d = null;  // the resource service is not up yet
            }
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            Backend.Current.Log(0, LogLevel.Debug, $"Kiln.Items: the game's names could not be read: {e.Message}");
            d = null;
        }
        lock (Gate)
        {
            _data = d;
            _loading = null;
            _failedAt = d == null ? Environment.TickCount64 : 0;
            _failures = d == null ? _failures + 1 : 0;
        }
    }

    private static byte[]? One(string type)
    {
        if (GameTypes.Find(type) is not { } t) return null;
        foreach (var id in ResourceStore.List(t.Hash))
            if (id.Part == 0) return ResourceStore.Read(id);
        return null;
    }

    /// <summary>The game's item list is read (this starts the read; <paramref name="wait"/>: on a worker, wait for it up to
    /// 10 s). Since 0.2.0-dev.31.13 (the voxel material labels wait for it).</summary>
    public static bool DataReady(bool wait = false) => Data(wait) != null;

    /// <summary>The GUID of a game item (an ItemInfo of the game's own data), from any thread. With <paramref name="wait"/> (a
    /// worker thread) the first call reads the data now; else null until it is read.</summary>
    public static Guid? ItemGuid(ItemId id, bool wait = false) =>
        Data(wait) is { } d && d.Items.TryGetValue(id.Value, out var g) ? g : null;

    /// <summary>The text of a loca tag in <see cref="Language"/>; null while the table loads (started here), on a server
    /// (no texts in its data), or for an unknown tag.</summary>
    public static string? Text(uint tag)
    {
        if (tag == 0) return null;
        var lang = Language;
        lock (Gate)
        {
            if (_texts is { } t && t.Language == lang) return t.Texts is { } m && m.TryGetValue(tag, out var s) && s.Length > 0 ? s : null;
            // a table that could not be read (the game has not opened its content index yet): again after 5 s, doubling, at most 5 min
            if (_textsFailed.Language == lang && Environment.TickCount64 - _textsFailed.At < RetryDelay.Of(_textsFailed.Failures, 5_000, 300_000))
                return null;
            if (_textsLoading == null && Data() is { Collection: not null } d)
            {
                var want = lang;
                _textsLoading = Task.Run(() => LoadTexts(d, want));
            }
        }
        return null;
    }

    private static void LoadTexts(Loaded d, string lang)
    {
        Dictionary<uint, string>? table = null;
        try
        {
            if (d.LanguageValues.TryGetValue(lang, out var v) || d.LanguageValues.TryGetValue("en-us", out v))
                if (GameNameData.LanguageBlob(d.Collection, d.Layout, v) is { } hash && ResourceStore.ReadBlob(hash) is { } blob)
                    table = GameNameData.Table(blob, d.Layout);
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            Backend.Current.Log(0, LogLevel.Debug, $"Kiln.Items: the game's {lang} texts could not be read: {e.Message}");
        }
        lock (Gate)
        {
            if (table != null)
            {
                _texts = (lang, table);
                _textsFailed = default;
            }
            else _textsFailed = (lang, Environment.TickCount64, _textsFailed.Language == lang ? _textsFailed.Failures + 1 : 1);
            _textsLoading = null;
        }
    }

    /// <summary>A game item's name in <see cref="Language"/>, or null (not a game item, not loaded yet, a server).</summary>
    public static string? ItemName(ItemId id)
    {
        if (Data() is not { } d || !d.Items.TryGetValue(id.Value, out var guid)) return null;
        uint tag;
        lock (Gate)
            if (!ItemTags.TryGetValue(id.Value, out tag))
            {
                var bytes = ResourceStore.Read(new ResourceId(guid, ItemInfoType, 0));
                ItemTags[id.Value] = tag = bytes is null ? 0 : GameNameData.ItemNameTag(bytes, d.Layout);
            }
        return Text(tag);
    }

    /// <summary>A workshop of the game's data: its name tag and the item that places its station.</summary>
    public static (uint NameTag, ItemId Item)? Workshop(uint workshop) =>
        Data() is { } d && d.Workshops.TryGetValue(workshop, out var w) ? (w.NameTag, new ItemId(w.Item)) : null;

    /// <summary>A recipe of the game's data: its recipeName tag (0 when it has none) and its first output.</summary>
    public static (uint NameTag, ItemId FirstOutput)? Recipe(uint recipe) =>
        Data() is { } d && d.Recipes.TryGetValue(recipe, out var r) ? (r.NameTag, new ItemId(r.FirstOutput)) : null;

    /// <summary>Starts reading what the names need (the data and this language's texts) in the background.</summary>
    public static void Preload() => _ = Text(1);
}
