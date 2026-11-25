// SPDX-License-Identifier: MIT
// The game's text tables per language, read from its own resources (since 0.2.0-dev.31.13, mod API 0.2.22): what
// Content.TextIn, Content.TryGetTexts / LoadTextsAsync, Items.Description and Items.DisplayNameIn(item, language) answer from.
//
// Where they come from (types from the game's reflection, kiln.types; bytes from kiln.assets; checked on client 26430625):
//  - keen::LocaTagCollectionResource (one resource, game clients only): `languages`, a BlobArray of
//    keen::LocaTagCollectionResourceLanguage {LanguageId language @0, ResourceContentReference data @8} (40 B). The client
//    ships 15: en-us, de-de, fr-fr, it-it, es-es, ru-ru, pl-pl, ja-jp, ko-kr, pt-br, th-th, tr-tr, zh-cn, zh-tw, uk-ua.
//  - each language's content blob (data.hash, 16 bytes): a BlobArray at offset 0 of keen::LocaTagResource
//    {LocaTagId id @0, string text @4, ...} (24 B). A text is UTF-8 with the game's markup (<b>, <lorename>, ...).
//  - a dedicated server's data has neither the resource nor its blobs (server 26430253: docs/re/world-status.md section 12).
// Reads go through kiln.assets (resource_read; blob_read_resident, which honours registered blob overrides). Nothing here
// waits on the caller's thread unless it asks to (LoadAsync): the first question starts a background read.
using System.Buffers.Binary;
using System.Text;
using System.Text.RegularExpressions;

namespace Kiln;

/// <summary>The reflected offsets <see cref="GameTextData"/> reads with (<see cref="Measured"/>: client 26430625's).</summary>
internal sealed record GameTextLayout(int Languages, int LanguageStride, int LanguageValue, int LanguageData, int TagStride, int TagId, int TagText)
{
    /// <summary>Client 26430625 (tools/kfc types): what the tests use.</summary>
    public static readonly GameTextLayout Measured = new(0x20, 40, 0, 8, 24, 0, 4);

    /// <summary>The running game's offsets from kiln.types, or null when one of the types or fields is missing.</summary>
    public static GameTextLayout? Reflected()
    {
        if (!GameTypes.Available) return null;
        static int F(string type, string field) => GameTypes.Find(type)?.Field(field)?.Offset ?? -1;
        static int S(string type) => GameTypes.Find(type)?.Size ?? -1;
        var l = new GameTextLayout(
            F("keen::LocaTagCollectionResource", "languages"), S("keen::LocaTagCollectionResourceLanguage"),
            F("keen::LocaTagCollectionResourceLanguage", "language"), F("keen::LocaTagCollectionResourceLanguage", "data"),
            S("keen::LocaTagResource"), F("keen::LocaTagResource", "id"), F("keen::LocaTagResource", "text"));
        return l.Valid ? l : null;
    }

    // every field read must fit inside its row: the language value (4 bytes), its blob hash (16), the tag id (4), the text (8)
    public bool Valid => Languages >= 0 && LanguageValue >= 0 && LanguageData >= 0 && TagId >= 0 && TagText >= 0 &&
                         LanguageStride >= LanguageValue + 4 && LanguageStride >= LanguageData + 16 &&
                         TagStride >= TagId + 4 && TagStride >= TagText + 8;
}

/// <summary>Pure readers of the game's loca bytes, as kiln.assets gives them. Host-tested.</summary>
internal static class GameTextData
{
    /// <summary>The most texts one table keeps, and the longest text (longer ones are left out, never cut).</summary>
    public const int MaxTexts = 2_000_000, MaxTextBytes = 1 << 16;

    private static readonly Regex Markup = new("<[^<>]{1,64}>", RegexOptions.Compiled | RegexOptions.CultureInvariant);

    /// <summary>A keen::BlobArray {i32 offset from the field, u32 count} at <paramref name="at"/>: where its elements start
    /// and how many there are; false when it does not fit the data.</summary>
    public static bool BlobArray(ReadOnlySpan<byte> d, int at, int stride, out int start, out int count)
    {
        start = count = 0;
        if (at < 0 || at > d.Length - 8 || stride <= 0) return false;
        var rel = BinaryPrimitives.ReadInt32LittleEndian(d[at..]);
        var n = BinaryPrimitives.ReadUInt32LittleEndian(d[(at + 4)..]);
        if (n == 0) return true;
        var s = (long)at + rel;
        if (s < 0 || n > int.MaxValue / stride || s + (long)n * stride > d.Length) return false;
        start = (int)s;
        count = (int)n;
        return true;
    }

    /// <summary>A keen::BlobString {i32 offset from the field, u32 length} at <paramref name="at"/>, trailing NULs taken
    /// off; null when it does not fit the data or is longer than <see cref="MaxTextBytes"/>.</summary>
    public static string? BlobString(ReadOnlySpan<byte> d, int at)
    {
        if (at < 0 || at > d.Length - 8) return null;
        var rel = BinaryPrimitives.ReadInt32LittleEndian(d[at..]);
        var n = BinaryPrimitives.ReadUInt32LittleEndian(d[(at + 4)..]);
        if (n == 0) return "";
        var s = (long)at + rel;
        if (s < 0 || n > MaxTextBytes || s + n > d.Length) return null;
        var bytes = d.Slice((int)s, (int)n);
        while (bytes.Length > 0 && bytes[^1] == 0) bytes = bytes[..^1];
        return Encoding.UTF8.GetString(bytes);
    }

    // A BlobString header (inside the data) whose text is inside the data too but longer than MaxTextBytes.
    private static bool TooLong(ReadOnlySpan<byte> d, int at)
    {
        if (at < 0 || at > d.Length - 8) return false;
        var n = BinaryPrimitives.ReadUInt32LittleEndian(d[(at + 4)..]);
        var s = (long)at + BinaryPrimitives.ReadInt32LittleEndian(d[at..]);
        return n > MaxTextBytes && s >= 0 && s + n <= d.Length;
    }

    /// <summary>FNV-1a-32 over 16 bytes: a LocaTag GUID's tag id, an item GUID's item id. 0 for an all-zero reference.</summary>
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

    /// <summary>The languages of a LocaTagCollectionResource: each one's keen::LanguageId value and its blob's content
    /// hash, in the resource's order. Null when the array does not read.</summary>
    public static List<(long Value, byte[] Hash)>? Languages(ReadOnlySpan<byte> collection, GameTextLayout l)
    {
        if (!l.Valid || !BlobArray(collection, l.Languages, l.LanguageStride, out var start, out var n) || n > 4096) return null;
        var list = new List<(long, byte[])>(n);
        for (var i = 0; i < n; i++)
        {
            var e = start + i * l.LanguageStride;
            list.Add((BinaryPrimitives.ReadInt32LittleEndian(collection[(e + l.LanguageValue)..]), collection.Slice(e + l.LanguageData, 16).ToArray()));
        }
        return list;
    }

    /// <summary>One language's texts as the blob holds them: tag id -> text with the game's markup, trailing NULs off.
    /// A row with tag 0, and a text longer than <see cref="MaxTextBytes"/>, are left out. Null when the blob does not read
    /// as a table (a row or a string outside it): never a table with wrong texts.</summary>
    public static Dictionary<uint, string>? Table(ReadOnlySpan<byte> blob, GameTextLayout l)
    {
        if (!l.Valid || !BlobArray(blob, 0, l.TagStride, out var start, out var n) || n > MaxTexts) return null;
        var t = new Dictionary<uint, string>(n);
        for (var i = 0; i < n; i++)
        {
            var r = start + i * l.TagStride;
            var id = BinaryPrimitives.ReadUInt32LittleEndian(blob[(r + l.TagId)..]);
            if (TooLong(blob, r + l.TagText)) continue;   // inside the blob, but longer than a text Kiln keeps: left out
            var s = BlobString(blob, r + l.TagText);
            if (s is null) return null;
            if (id != 0) t[id] = s;
        }
        return t;
    }

    /// <summary>A text without the game's markup tags (<c>&lt;b&gt;</c>, <c>&lt;lorename&gt;</c>), trimmed.</summary>
    public static string Plain(string s) => s.IndexOf('<') < 0 ? s.Trim() : Markup.Replace(s, "").Trim();

    /// <summary>"De_De" (a keen::LanguageId value's name) -> "de-de".</summary>
    public static string LanguageKey(string enumName) => enumName.Replace('_', '-').ToLowerInvariant();
}

/// <summary>What <see cref="GameTextStore"/> reads its tables from: the game's resources, or a test kit's script.</summary>
internal interface IGameTextSource
{
    /// <summary>Why there are no game texts here at all (a dedicated server, no kiln.assets), or null when there may be.</summary>
    string? Unavailable { get; }

    /// <summary>The languages the game ships, as keys ("en-us", "de-de"), in the game's order; empty while unknown.</summary>
    IReadOnlyList<string> Languages { get; }

    /// <summary>One language's table with the game's markup, or null with the reason. Runs on a worker.</summary>
    Dictionary<uint, string>? Read(string language, out string reason);
}

/// <summary>The game's text tables by language (see the file header). Any thread; only <see cref="LoadAsync"/> waits.</summary>
internal static class GameTextStore
{
    /// <summary>Why a dedicated server has no texts: the one sentence the API and its capability row give.</summary>
    public const string ServerReason =
        "a dedicated server's game data has no texts (the game ships them with its client only); " +
        "English from a Kiln catalog's loca section is the server's alternative (Texts.Get)";

    /// <summary>The most languages kept at once (a table is a few MB); the least recently used goes first.</summary>
    public const int MaxLanguages = 4;

    private sealed class Entry
    {
        public Dictionary<uint, string>? Texts;
        public string Reason = "";
        public Task? Loading;
        public long FailedAt;
        public int Failures;
        public long Used;
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<string, Entry> Entries = new(StringComparer.Ordinal);
    private static IGameTextSource? _override;
    private static long _clock;
    private static long _generation;

    /// <summary>Test seam (the test kit, host tests): read from <paramref name="source"/> instead of the game; null: the
    /// game's resources again. Drops every table read so far.</summary>
    internal static void UseSource(IGameTextSource? source)
    {
        lock (Gate)
        {
            _override = source;
            Entries.Clear();
            _generation++;
        }
    }

    /// <summary>Test seam: the source's data changed (a test kit's script): drops every table read so far.</summary>
    internal static void Invalidate()
    {
        lock (Gate)
        {
            Entries.Clear();
            _generation++;
        }
    }

    /// <summary>Why a language has no table: the game does not ship it (the same sentence for the game and a test kit).</summary>
    public static string NotShipped(string language, IReadOnlyList<string> shipped) =>
        $"the game ships no texts in \"{language}\" (it has {(shipped.Count == 0 ? "none" : string.Join(", ", shipped))})";

    internal static IGameTextSource Source { get { lock (Gate) return _override ?? ResourceGameTextSource.Instance; } }

    /// <summary>Why there are no game texts here, or null.</summary>
    public static string? Unavailable => Source.Unavailable;

    /// <summary>The languages the game ships ("en-us", ...); empty on a server or while unknown.</summary>
    public static IReadOnlyList<string> Languages => Source.Languages;

    /// <summary>A language as the store keys it: null or empty is the player's game language (<c>GameLanguage.Current</c>;
    /// "en-us" on a server); "De_De", "de_DE" and "DE-DE" are "de-de"; a primary language alone ("de", "pt") is the first
    /// shipped language with it ("de-de", "pt-br"), or stays as it is when none has it.</summary>
    public static string Resolve(string? language)
    {
        var key = string.IsNullOrWhiteSpace(language) ? GameLanguageSource.Current() : ModLocale.Normalize(language);
        if (key.IndexOf('-') >= 0) return key;
        foreach (var l in Languages)
            if (l.StartsWith(key + "-", StringComparison.Ordinal)) return l;
        return key;
    }

    /// <summary>The table of <paramref name="language"/> (a key from <see cref="Resolve"/>) when it is read: its texts,
    /// or null texts and the reason it could not be read. Null while it is read (this starts the read in the background),
    /// and while a failed read waits to be tried again (5 s, doubling to 5 minutes) after it was answered once.</summary>
    public static (IReadOnlyDictionary<uint, string>? Texts, string Reason)? TryGet(string language)
    {
        var source = Source;
        if (source.Unavailable is { } why) return (null, why);
        lock (Gate)
        {
            var e = EntryOf(language);
            if (e.Texts != null) return (e.Texts, "");
            if (e.Loading != null) return null;
            if (e.FailedAt != 0)
            {
                if (Environment.TickCount64 - e.FailedAt < RetryDelay.Of(e.Failures, 5_000, 300_000)) return (null, e.Reason);
            }
            Start(e, language, source);
            return null;
        }
    }

    /// <summary>Reads <paramref name="language"/>'s table (once; the next calls answer from memory) and completes with
    /// it, or with null texts and the reason. Never faults: a read that throws is a reason.</summary>
    public static async Task<(IReadOnlyDictionary<uint, string>? Texts, string Reason)> LoadAsync(string language, CancellationToken ct)
    {
        for (var attempt = 0; attempt < 2; attempt++)
        {
            var source = Source;
            if (source.Unavailable is { } why) return (null, why);
            Task loading;
            Entry e;
            lock (Gate)
            {
                e = EntryOf(language);
                if (e.Texts != null) return (e.Texts, "");
                // an explicit load does not wait out a failed read's back-off: it is the caller's own question
                e.Loading ??= Start(e, language, source);
                loading = e.Loading;
            }
            await loading.WaitAsync(ct).ConfigureAwait(false);
            lock (Gate)
                if (e.Loading == null && (e.Texts != null || e.FailedAt != 0))
                    return (e.Texts, e.Texts != null ? "" : e.Reason);
        }
        return (null, "the game's texts were replaced while they were read; ask again");
    }

    /// <summary>The text of <paramref name="tag"/> in <paramref name="language"/> with the game's markup, or null: an unknown
    /// tag, an empty text, a table not read yet (this starts the read), no texts here.</summary>
    public static string? Text(uint tag, string language)
    {
        if (tag == 0) return null;
        return TryGet(language) is { Texts: { } t } && t.TryGetValue(tag, out var s) && s.Length > 0 ? s : null;
    }

    // Under Gate.
    private static Entry EntryOf(string language)
    {
        if (!Entries.TryGetValue(language, out var e))
        {
            Entries[language] = e = new Entry();
            // keep the few most recently used tables; a load that runs is never dropped
            while (Entries.Count > MaxLanguages && Entries.Where(x => x.Value.Loading == null && x.Key != language)
                       .OrderBy(x => x.Value.Used).Select(x => x.Key).FirstOrDefault() is { } old)
                Entries.Remove(old);
        }
        e.Used = ++_clock;
        return e;
    }

    // Under Gate.
    private static Task Start(Entry e, string language, IGameTextSource source)
    {
        var generation = _generation;
        e.Loading = Task.Run(() =>
        {
            Dictionary<uint, string>? t = null;
            string reason;
            try
            {
                t = source.Read(language, out reason);
            }
            catch (Exception x) when (x is not OutOfMemoryException)
            {
                reason = $"the game's {language} texts could not be read: {x.Message}";
            }
            lock (Gate)
            {
                if (generation != _generation) return;  // the source was replaced meanwhile (tests): publish nothing
                e.Loading = null;
                if (t != null)
                {
                    e.Texts = t;
                    e.Reason = "";
                    e.FailedAt = 0;
                    e.Failures = 0;
                }
                else
                {
                    e.Reason = reason.Length > 0 ? reason : $"the game's {language} texts could not be read";
                    e.FailedAt = Environment.TickCount64;
                    e.Failures++;
                }
            }
        });
        return e.Loading;
    }

    internal static void ResetForTests() => UseSource(null);
}

/// <summary>The game's own resources as a text source (kiln.assets, kiln.types).</summary>
internal sealed class ResourceGameTextSource : IGameTextSource
{
    public static readonly ResourceGameTextSource Instance = new();

    private readonly object _gate = new();
    private (GameTextLayout Layout, List<(long Value, byte[] Hash)> Languages, Dictionary<long, string> Keys)? _collection;
    private long _failedAt;

    public string? Unavailable
    {
        get
        {
            if (Mods.Side != ModSide.Client) return GameTextStore.ServerReason;
            if (!ResourceStore.Available) return "kiln.assets' resource reads are off here (row assets.resources)";
            if (!GameTypes.Available) return "kiln.types has no game types here";
            return null;
        }
    }

    public IReadOnlyList<string> Languages
    {
        get
        {
            if (Unavailable != null || Collection() is not { } c) return [];
            return [.. c.Languages.Select(l => c.Keys.TryGetValue(l.Value, out var k) ? k : null).OfType<string>()];
        }
    }

    // The collection resource (640 bytes) and the LanguageId names, read once; a failed read is tried again after 5 s.
    private (GameTextLayout Layout, List<(long Value, byte[] Hash)> Languages, Dictionary<long, string> Keys)? Collection()
    {
        lock (_gate)
        {
            if (_collection is { } have) return have;
            if (_failedAt != 0 && Environment.TickCount64 - _failedAt < 5_000) return null;
            _failedAt = Environment.TickCount64;
            if (GameTextLayout.Reflected() is not { } layout || GameTypes.Find("keen::LocaTagCollectionResource") is not { } type) return null;
            byte[]? bytes = null;
            foreach (var id in ResourceStore.List(type.Hash))
                if (id.Part == 0 && (bytes = ResourceStore.Read(id)) != null) break;
            if (bytes == null || GameTextData.Languages(bytes, layout) is not { } languages) return null;
            var keys = new Dictionary<long, string>();
            foreach (var (name, value) in GameTypes.Find("keen::LanguageId")?.EnumValues ?? [])
                keys.TryAdd(value, GameTextData.LanguageKey(name));
            _collection = (layout, languages, keys);
            _failedAt = 0;
            return _collection;
        }
    }

    public Dictionary<uint, string>? Read(string language, out string reason)
    {
        if (Unavailable is { } why)
        {
            reason = why;
            return null;
        }
        if (Collection() is not { } c)
        {
            reason = "the game's loca collection (keen::LocaTagCollectionResource) could not be read yet";
            return null;
        }
        foreach (var (value, hash) in c.Languages)
        {
            if (!c.Keys.TryGetValue(value, out var key) || key != language) continue;
            if (ResourceStore.ReadBlob(hash) is not { } blob)
            {
                reason = $"the game's {language} text blob is not readable yet (the game has not opened its content index)";
                return null;
            }
            var table = GameTextData.Table(blob, c.Layout);
            reason = table == null ? $"the game's {language} text blob does not read as a text table on this build" : "";
            return table;
        }
        reason = GameTextStore.NotShipped(language, Languages);
        return null;
    }
}
