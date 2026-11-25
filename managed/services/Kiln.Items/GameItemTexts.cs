// SPDX-License-Identifier: MIT
// An item type's texts in any language the game ships (since 0.2.0-dev.31.13, mod API 0.2.22): Items.TextTags,
// Items.Description, Items.DisplayName(item, language). For XHL's EmberCodex (a codex page in a chosen language) and
// Creative Mode Fix (search over names and descriptions).
//
// Where they come from: keen::ItemInfo's `name` (+0x48), `caption` (+0x58), `description` (+0x68) and `lore` (+0x78) are
// LocaTag GUIDs (NameLocaTag, CaptionLocaTag, DescriptionLocaTag, LoreLocaTag; client 26430625, offsets from kiln.types at
// run time); a tag's id is FNV-1a-32 of its 16 bytes, and Kiln.Api's GameTextStore holds the game's table per language.
// A new item (kiln.content) has its texts in Kiln, not in the game's tables: its definition's, found by reflection
// (DefinedNames), as Items.DisplayName does.
namespace Kiln;

/// <summary>The loca tag ids of an item type's texts: FNV-1a-32 of the <c>keen::ItemInfo</c> LocaTag GUIDs (the ids
/// <c>Texts.TagOf</c> and the game's tables use), 0 where the item has none. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
/// <param name="Name">The name (<c>ItemInfo.name</c>).</param>
/// <param name="Caption">The line under the name (<c>ItemInfo.caption</c>).</param>
/// <param name="Description">The description (<c>ItemInfo.description</c>).</param>
/// <param name="Lore">The lore text (<c>ItemInfo.lore</c>).</param>
public readonly record struct ItemTextTags(uint Name, uint Caption, uint Description, uint Lore);

/// <summary>Reads an item type's tags from its ItemInfo (Kiln.Items' GameNames knows the game items' GUIDs).</summary>
internal static class GameItemTexts
{
    private const uint ItemInfoType = 0xb5ce8765;

    /// <summary>Test seam (the test kit, host tests): an item's tags; null: the game's ItemInfo again.</summary>
    internal static Func<ItemId, ItemTextTags?>? Override;

    private static readonly object Gate = new();
    private static readonly Dictionary<uint, ItemTextTags?> Cache = new();
    private static (int Name, int Caption, int Description, int Lore)? _layout;

    /// <summary>The ItemInfo offsets of the four tags (client 26430625: 0x48, 0x58, 0x68, 0x78).</summary>
    internal static readonly (int Name, int Caption, int Description, int Lore) Measured = (0x48, 0x58, 0x68, 0x78);

    private static (int Name, int Caption, int Description, int Lore)? Layout()
    {
        lock (Gate)
        {
            if (_layout is { } l) return l;
            if (!GameTypes.Available || GameTypes.Find("keen::ItemInfo") is not { } t) return null;
            int F(string f) => t.Field(f)?.Offset ?? -1;
            var r = (F("name"), F("caption"), F("description"), F("lore"));
            if (r.Item1 < 0) return null;  // without a name tag there is nothing to read; a missing other field stays 0
            _layout = r;
            return r;
        }
    }

    /// <summary>The tags in an ItemInfo's bytes at the given offsets (pure; host-tested). A field outside the bytes, or one
    /// the build does not have (offset -1), is 0.</summary>
    internal static ItemTextTags Parse(ReadOnlySpan<byte> itemInfo, (int Name, int Caption, int Description, int Lore) l)
    {
        uint At(ReadOnlySpan<byte> b, int off) => off >= 0 && off + 16 <= b.Length ? GameTextData.Fnv16(b.Slice(off, 16)) : 0;
        return new ItemTextTags(At(itemInfo, l.Name), At(itemInfo, l.Caption), At(itemInfo, l.Description), At(itemInfo, l.Lore));
    }

    /// <summary>A game item's tags, or null: not a game item (a new item, an unknown id), the game's item list not read yet
    /// (this starts the read), no resources here.</summary>
    public static ItemTextTags? Of(ItemId item)
    {
        if (item.Value == 0) return null;
        if (Override is { } o) return o(item);
        lock (Gate)
            if (Cache.TryGetValue(item.Value, out var have)) return have;
        if (GameNames.ItemGuid(item) is not { } guid || Layout() is not { } l) return null;
        var bytes = ResourceStore.Read(new ResourceId(guid, ItemInfoType, 0));
        ItemTextTags? tags = bytes is null ? null : Parse(bytes, l);
        lock (Gate) Cache[item.Value] = tags;
        return tags;
    }

    internal static void ResetForTests()
    {
        lock (Gate)
        {
            Override = null;
            Cache.Clear();
            _layout = null;
        }
    }

    /// <summary>A null from <see cref="Of"/> is final: the game's item list is read, or cannot be here (no resources, no
    /// types, a test's script). <paramref name="wait"/>: on a worker, wait for the read up to 10 s.</summary>
    public static bool Ready(bool wait = false) =>
        Override != null || !ResourceStore.Available || !GameTypes.Available || GameNames.DataReady(wait);

    /// <summary>The texts' language for an answer: <see cref="GameTextStore.Resolve"/>.</summary>
    public static string Language(string? language) => GameTextStore.Resolve(language);

    /// <summary>English, where Kiln's shipped English names may answer.</summary>
    public static bool IsEnglish(string language) => language is "en" or "en-us" or "en-gb";

    /// <summary>The plain text (no markup, trimmed) of a tag in a language, or null for none or an empty one.</summary>
    public static string? Plain(uint tag, string language) =>
        GameTextStore.Text(tag, language) is { } s && GameTextData.Plain(s) is { Length: > 0 } p ? p : null;
}

public static unsafe partial class Items
{
    /// <summary>
    /// The loca tag ids of a game item type's texts (name, caption, description, lore), read from its <c>ItemInfo</c>: look
    /// them up in any language with <c>Content.TextIn(tags.Description, "de-de")</c> or enumerate a whole table with
    /// <c>Content.LoadTextsAsync</c>. Works on a dedicated server too (its data has the items, not the texts). Null for an
    /// id that is no game item (a new item's texts are Kiln's: <see cref="Description(ItemId, string?)"/>), and until the
    /// game's item list has been read (the first call starts that in the background) or without kiln.assets' resources.
    /// Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    [AnyThread]
    public static ItemTextTags? TextTags(ItemId item)
    {
        try
        {
            return GameItemTexts.Of(item);
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException or KilnUnavailableException)
        {
            return null;
        }
    }

    /// <summary>The item's description in the player's game language: <see cref="Description(ItemId, string?)"/> with
    /// <c>null</c>. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    [AnyThread]
    public static string? Description(ItemId item) => Description(item, null);

    /// <summary>
    /// The item's description in <paramref name="language"/> without the game's markup: a game item's from the game's own
    /// text table of that language, a new item's (kiln.content) from its definition's texts (that language, its primary
    /// language, <c>"*"</c>, English; else its base item's). <paramref name="language"/> is a key the game ships
    /// (<c>"de-de"</c>; <c>"de"</c> or <c>"De_De"</c> work too: <c>Content.TextLanguages</c> lists them); null is the player's
    /// game language. Null when the item has no description, the language's table is not read yet (the first call starts
    /// that in the background: <c>await Content.LoadTextsAsync(language)</c> first to have it at once), and on a dedicated
    /// server, whose data has no texts (there <c>Content.TextIn(Items.TextTags(item)?.Description ?? 0, "en-us")</c> answers
    /// from a Kiln catalog's English when the server has one). For the markup (<c>&lt;b&gt;</c>, ...) use
    /// <see cref="TextTags"/> with <c>Content.TextIn</c>. Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    /// <remarks>Any thread, also a draw callback: it never waits for a read.</remarks>
    [AnyThread]
    public static string? Description(ItemId item, string? language)
    {
        if (item.Value == 0) return null;
        try
        {
            var lang = GameItemTexts.Language(language);
            if (GameItemTexts.Of(item) is { } tags) return GameItemTexts.Plain(tags.Description, lang);
            if (GameNames.ItemGuid(item) is null) return DefinedNames.DescriptionOf(item.Value, lang);
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException or KilnUnavailableException)
        {
            // no description
        }
        return null;
    }

    /// <summary>
    /// The item's name in <paramref name="language"/> (a key the game ships, <c>"de-de"</c>; null: the player's game
    /// language), as <see cref="DisplayName(ItemId)"/> gives it for the player's language: a game item's from the game's
    /// own table of that language, a new item's from its definition. Unlike <see cref="DisplayName(ItemId)"/> it does not
    /// answer English for another language: null until that language's table is read (the first call starts that in the
    /// background) and on a dedicated server, except for English (<c>"en-us"</c>, <c>"en-gb"</c>), where the English name
    /// Kiln ships answers meanwhile and on a server. Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    /// <remarks>Any thread, also a draw callback: it never waits for a read.</remarks>
    [AnyThread]
    public static string? DisplayName(ItemId item, string? language)
    {
        if (item.Value == 0) return null;
        var lang = "en-us";
        try
        {
            lang = GameItemTexts.Language(language);
            if (GameItemTexts.Of(item) is { } tags && GameItemTexts.Plain(tags.Name, lang) is { } name) return name;
            if (GameNames.ItemGuid(item) is null && DefinedNames.Of(item.Value, lang) is { Length: > 0 } defined) return defined;
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException or KilnUnavailableException)
        {
            // the English name below, for English
        }
        return GameItemTexts.IsEnglish(lang) ? ItemCatalog.ShippedName(item.Value) : null;
    }
}
