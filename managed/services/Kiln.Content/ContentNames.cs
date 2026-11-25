// SPDX-License-Identifier: MIT
// kiln.content's new items as a game client's UI needs them (0.2.0-dev.29.15): their name in a language (Kiln.Items'
// Items.DisplayName finds NameOf by reflection) and the UI texture the game draws for them (Kiln.Ui's item icons). Read from
// kiln.content's listing of definitions (definitions(): "displayName", the definition's "texts", "iconGuid", "baseGuid"),
// in the background: an answer never waits for the listing, which takes kiln.content's store lock.
using System.Text.Json;

namespace Kiln;

internal static class ContentNames
{
    /// <summary>One new item: its GUID, the UI texture the game looks its icon up under while kiln.assets serves no icon of
    /// its own (its base item's, or its "icon" item's; null until it is in a world), the item it was cloned from, its
    /// display name, and its names per language as the definition gives them.</summary>
    internal sealed record Def(uint Id, Guid Guid, Guid? IconGuid, Guid? BaseGuid, string? DisplayName,
        IReadOnlyDictionary<string, string> Names, bool HasIconFile)
    {
        /// <summary>Its descriptions per language as the definition gives them (since 0.2.0-dev.31.13): the texts' and, as
        /// <c>"*"</c>, the definition's own <c>description</c>. An empty one is "no description" (not the base item's).</summary>
        public IReadOnlyDictionary<string, string> Descriptions { get; init; } = new Dictionary<string, string>();
    }

    private static readonly object Gate = new();
    private static Dictionary<uint, Def> _defs = new();
    private static long _readAt;      // Environment.TickCount64 of the last read, 0: never
    private static Task? _reading;
    private static string? _lastListing;
    private static int _unchanged;    // reads in a row that found the same listing (or none)

    /// <summary>How long a miss waits before the listing is read again: 2 s after a read that found something new, then
    /// twice as long after each read that found the same listing (an id that is no new item: a game item drawn or named, an
    /// unknown id), at most 2 minutes. A changed listing starts over at 2 s.</summary>
    internal static long Stale => 2000 + RetryDelay.Of(_unchanged, 2000, 118_000);

    /// <summary>Tests: the listing to read instead of kiln.content's (null: kiln.content's).</summary>
    internal static Func<string?>? ListingOverride;

    /// <summary>Tests: the clock (Environment.TickCount64).</summary>
    internal static Func<long> Clock = () => Environment.TickCount64;

    /// <summary>The definition of a new item with this id, or null (none, or the listing is being read: asked again, it
    /// answers). A miss reads the listing again in the background, at most every <see cref="Stale"/>.</summary>
    public static Def? Find(uint id)
    {
        lock (Gate)
        {
            if (_defs.TryGetValue(id, out var d)) return d;
            if (_reading == null && (_readAt == 0 || Clock() - _readAt >= Stale))
                _reading = Task.Run(Read);
            return null;
        }
    }

    /// <summary><see cref="Find"/> for a worker thread that may wait: a miss reads the listing now (at most every
    /// <see cref="Stale"/>) and looks again.</summary>
    public static Def? FindNow(uint id)
    {
        Task running;
        // one look under the lock: a read that ended between a miss and a second look would leave no read to wait for
        // and a fresh read time, and the item that read found would be answered as missing
        lock (Gate)
        {
            if (_defs.TryGetValue(id, out var d)) return d;
            if (_reading == null)
            {
                if (_readAt != 0 && Clock() - _readAt < Stale) return null;
                _reading = Task.Run(Read);
            }
            running = _reading;
        }
        running.Wait(TimeSpan.FromSeconds(5));
        lock (Gate) return _defs.GetValueOrDefault(id);
    }

    /// <summary>Reads the listing now (a worker thread; tests).</summary>
    internal static void Read()
    {
        Dictionary<uint, Def>? next = null;
        string? json = null;
        try
        {
            json = ListingOverride is { } o ? o() : NewContent.Available ? NewContent.DefinitionsJson() : null;
            if (json != null) next = Parse(json);
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            Backend.Current.Log(0, LogLevel.Debug, $"Kiln.Content: new items' names and icons: the listing could not be read: {e.Message}");
        }
        lock (Gate)
        {
            if (next != null) _defs = next;
            _unchanged = json != null && json != _lastListing ? 0 : _unchanged + 1;
            _lastListing = json;
            _readAt = Clock();
            _reading = null;
        }
    }

    internal static void ResetForTests()
    {
        lock (Gate)
        {
            _defs = new();
            _readAt = 0;
            _reading = null;
            _lastListing = null;
            _unchanged = 0;
            ListingOverride = null;
            Clock = () => Environment.TickCount64;
        }
    }

    /// <summary>The items of a definitions() listing.</summary>
    internal static Dictionary<uint, Def> Parse(string json)
    {
        var map = new Dictionary<uint, Def>();
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.ValueKind != JsonValueKind.Array) return map;
        foreach (var e in doc.RootElement.EnumerateArray())
        {
            if (Str(e, "kind") != "item" || !Guid.TryParse(Str(e, "guid"), out var guid)) continue;
            var id = NewContent.IdOf(guid);
            var names = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var descriptions = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            string? display = Str(e, "displayName");
            if (e.TryGetProperty("definition", out var body) && body.ValueKind == JsonValueKind.Object)
            {
                display ??= Str(body, "displayName");
                if (Str(body, "description") is { } all) descriptions["*"] = all;
                if (body.TryGetProperty("texts", out var texts) && texts.ValueKind == JsonValueKind.Object)
                    foreach (var t in texts.EnumerateObject())
                    {
                        if (t.Value.ValueKind != JsonValueKind.Object) continue;
                        if (Str(t.Value, "name") is { Length: > 0 } n) names[t.Name.Trim()] = n;
                        if (Str(t.Value, "description") is { } d) descriptions[t.Name.Trim()] = d;
                    }
            }
            map[id] = new Def(id, guid, Ref(Str(e, "iconGuid")), Ref(Str(e, "baseGuid")), display is { Length: > 0 } ? display : null, names,
                Str(e, "iconFile") is { Length: > 0 }) { Descriptions = descriptions };
        }
        return map;
    }

    private static string? Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private static Guid? Ref(string? text) => Guid.TryParse(text, out var g) && g != Guid.Empty ? g : null;

    /// <summary>A definition's name for a game language ("de-de"): that language, its primary language ("de"), every language
    /// ("*"), English ("en", "en-us"), then the display name; null when the definition names none (the item keeps its base
    /// item's name then).</summary>
    internal static string? NameFor(Def d, string language)
    {
        var lang = (language ?? "").Trim().Replace('_', '-').ToLowerInvariant();
        var dash = lang.IndexOf('-');
        foreach (var key in new[] { lang, dash > 0 ? lang[..dash] : "", "*", "en", "en-us" })
            if (key.Length > 0 && d.Names.TryGetValue(key, out var n)) return n;
        return d.DisplayName;
    }

    /// <summary>A definition's description for a game language, in <see cref="NameFor"/>'s order (that language, its primary
    /// language, <c>"*"</c>, English): the text, <c>""</c> for "no description", or null when the definition gives none (the
    /// item keeps its base item's then). Since 0.2.0-dev.31.13.</summary>
    internal static string? DescriptionFor(Def d, string language)
    {
        var lang = (language ?? "").Trim().Replace('_', '-').ToLowerInvariant();
        var dash = lang.IndexOf('-');
        foreach (var key in new[] { lang, dash > 0 ? lang[..dash] : "", "*", "en", "en-us" })
            if (key.Length > 0 && d.Descriptions.TryGetValue(key, out var text)) return text;
        return null;
    }

    /// <summary>Kiln.Items' <c>Items.Description</c> asks this (by reflection) for a new item's description: its definition's,
    /// else its base item's in the same language. Null: none, or no new item (yet) by this id. Since 0.2.0-dev.31.13.</summary>
    internal static string? DescriptionOf(uint id, string language)
    {
        if (Find(id) is not { } d) return null;
        if (DescriptionFor(d, language) is { } text) return text.Trim() is { Length: > 0 } t ? t : null;
        if (d.BaseGuid is not { } b || _depth > 4) return null;
        _depth++;
        try
        {
            return Items.Description(new ItemId(NewContent.IdOf(b)), language);
        }
        finally
        {
            _depth--;
        }
    }

    [ThreadStatic] private static int _depth;

    /// <summary>Kiln.Items' <c>Items.DisplayName</c> asks this (by reflection) for a new item's name: its definition's, else its
    /// base item's (a new item keeps the base's texts where its definition gives none). Null: no new item (yet) by this id.</summary>
    internal static string? NameOf(uint id, string language)
    {
        if (Find(id) is not { } d) return null;
        if (NameFor(d, language) is { } n) return n;
        if (d.BaseGuid is not { } b || _depth > 4) return null;
        _depth++;
        try
        {
            return Items.DisplayName(new ItemId(NewContent.IdOf(b)));
        }
        finally
        {
            _depth--;
        }
    }
}
