// SPDX-License-Identifier: MIT
// The game's texts in a chosen language, read from its own resources (since 0.2.0-dev.31.13, mod API 0.2.22):
// Content.TextLanguages, Content.TextIn, Content.TryGetTexts, Content.LoadTextsAsync and GameTextTable. For XHL's EmberCodex
// (a codex page in a language the player picks), Easy HUD and Planting Assistant (labels by loca tag on any thread).
//
// Content.Text (0.2.0-dev.29.18) asks the game's own lookup in the player's language on the main thread; these read the
// language tables the game ships (Kiln.Api's GameTextStore: keen::LocaTagCollectionResource and its content blobs through
// kiln.assets), on any thread, in any of the client's 15 languages. A dedicated server's data has no texts: there English
// comes from a Kiln catalog's loca section (Texts) when the server has one, and every other language is unavailable with a
// reason. docs/kiln/services/content.md "Game texts in any language".
using System.Collections;
using System.Diagnostics.CodeAnalysis;

namespace Kiln;

/// <summary>Where a <see cref="GameTextTable"/>'s texts come from. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
public enum GameTextSource
{
    /// <summary>No texts: <see cref="GameTextTable.Reason"/> says why.</summary>
    None = 0,

    /// <summary>The game's own table of that language, from the game client's data.</summary>
    Game = 1,

    /// <summary>English from a Kiln catalog's <c>loca</c> section (<see cref="Texts"/>): a dedicated server's only texts.</summary>
    KilnCatalog = 2,
}

/// <summary>
/// One language's game texts: loca tag id -> text, with the game's markup (<c>&lt;b&gt;</c>, <c>&lt;lorename&gt;</c>, ...;
/// <see cref="GetPlain"/> strips it). Enumerate it for every text of the language. Immutable: a copy owned by the caller,
/// never game memory, safe on any thread. An unavailable table (<see cref="Available"/> false) is empty and says why.
/// Since 0.2.0-dev.31.13, mod API 0.2.22.
/// </summary>
public sealed class GameTextTable : IReadOnlyDictionary<uint, string>
{
    private static readonly IReadOnlyDictionary<uint, string> Empty = new Dictionary<uint, string>();
    private readonly IReadOnlyDictionary<uint, string> _texts;

    internal GameTextTable(string language, GameTextSource source, IReadOnlyDictionary<uint, string>? texts, string reason)
    {
        Language = language;
        _texts = source == GameTextSource.None || texts is null ? Empty : texts;
        Source = source == GameTextSource.None || texts is null ? GameTextSource.None : source;
        Reason = Source == GameTextSource.None ? (reason.Length > 0 ? reason : "no texts") : "";
    }

    /// <summary>The language, as the game keys it (<c>"de-de"</c>).</summary>
    public string Language { get; }

    /// <summary>Where the texts come from; <see cref="GameTextSource.None"/> when there are none.</summary>
    public GameTextSource Source { get; }

    /// <summary>The table has texts (<see cref="Source"/> is not <see cref="GameTextSource.None"/>).</summary>
    public bool Available => Source != GameTextSource.None;

    /// <summary>Why there are no texts ("" when <see cref="Available"/>): a dedicated server, a language the game does not
    /// ship, a table that did not read.</summary>
    public string Reason { get; }

    /// <summary>How many texts the table has.</summary>
    public int Count => _texts.Count;

    /// <summary>The text of a tag id (with markup).</summary>
    /// <exception cref="KeyNotFoundException">The table has no text for the tag.</exception>
    public string this[uint key] => _texts[key];

    /// <summary>Every tag id the table has a text for.</summary>
    public IEnumerable<uint> Keys => _texts.Keys;

    /// <summary>Every text (with markup).</summary>
    public IEnumerable<string> Values => _texts.Values;

    /// <summary>The table has a text for the tag id.</summary>
    public bool ContainsKey(uint key) => _texts.ContainsKey(key);

    /// <summary>The text of a tag id (with markup), when the table has one.</summary>
    public bool TryGetValue(uint key, [MaybeNullWhen(false)] out string value) => _texts.TryGetValue(key, out value);

    /// <summary>The text of a loca tag id with the game's markup, or null (no such text, an empty one).</summary>
    public string? Get(uint locaId) => locaId != 0 && _texts.TryGetValue(locaId, out var s) && s.Length > 0 ? s : null;

    /// <summary>The text of a LocaTag reference (a GUID, as <c>ItemInfo.name</c>), hashed as the game does.</summary>
    public string? Get(Guid locaTag) => locaTag == Guid.Empty ? null : Get(Texts.TagOf(locaTag));

    /// <summary>As <see cref="Get(uint)"/> without the game's markup tags, trimmed; null for none or an empty one.</summary>
    public string? GetPlain(uint locaId) => Get(locaId) is { } s && GameTextData.Plain(s) is { Length: > 0 } p ? p : null;

    /// <summary>Every (tag id, text) pair, with markup.</summary>
    public IEnumerator<KeyValuePair<uint, string>> GetEnumerator() => _texts.GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>The language, how many texts and where from; or why there are none.</summary>
    public override string ToString() => Available ? $"{Count} {Language} texts ({Source})" : $"no {Language} texts: {Reason}";
}

public static partial class Content
{
    /// <summary>The capability row of <see cref="TextIn(uint, string?)"/>, <see cref="TryGetTexts"/> and
    /// <see cref="LoadTextsAsync"/>: available on a game client whose kiln.assets reads resources (<c>assets.resources</c>)
    /// and whose kiln.types is here; unavailable with <c>wrong-side</c> on a dedicated server (its data has no texts; English
    /// may still come from a Kiln catalog, <see cref="GameTextSource.KilnCatalog"/>). Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    public const string GameTextsCapability = "content.game_texts";

    /// <summary>
    /// The languages the game ships texts in, as the game keys them (<c>"en-us"</c>, <c>"de-de"</c>, <c>"pt-br"</c>, ... 15 on
    /// client 26430625), in the game's order. On a dedicated server <c>["en-us"]</c> once a Kiln catalog's English is loaded
    /// (this starts that), else empty; empty too while the game's collection is not readable yet.
    /// Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    [AnyThread]
    public static IReadOnlyList<string> TextLanguages
    {
        get
        {
            LookupCaps.Rows.Ensure();
            if (GameTextStore.Unavailable is not null)
            {
                if (!OnServerWithCatalog()) return [];
                if (Texts.Loaded is not null) return ["en-us"];
                _ = Texts.Preload();
                return [];
            }
            return GameTextStore.Languages;
        }
    }

    /// <summary>
    /// One language's texts when they are read already, without waiting: <paramref name="language"/> is a key the game ships
    /// (<see cref="TextLanguages"/>; <c>"de"</c>, <c>"De_De"</c> and <c>"DE-de"</c> work too), null the player's game language
    /// (<c>"en-us"</c> on a server). Null while the table is read: the first call starts that in the background (about a
    /// quarter of a second for a client language); call again later, or <c>await</c> <see cref="LoadTextsAsync"/>. A table
    /// that cannot be read (a dedicated server for a language other than English, a language the game does not ship, no
    /// kiln.assets) comes back at once, unavailable, with its <see cref="GameTextTable.Reason"/>. Kiln keeps the four most
    /// recently used languages. Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    [AnyThread]
    public static GameTextTable? TryGetTexts(string? language)
    {
        LookupCaps.Rows.Ensure();
        var lang = GameTextStore.Resolve(language);
        if (GameTextStore.Unavailable is { } why && OnServerWithCatalog())
        {
            if (!IsCatalogLanguage(lang)) return new GameTextTable(lang, GameTextSource.None, null, why);
            if (Texts.Loaded is { } english) return new GameTextTable(lang, GameTextSource.KilnCatalog, english, "");
            var load = Texts.Preload();
            if (!load.IsCompleted) return null;
            return Texts.Loaded is { } e2 ? new GameTextTable(lang, GameTextSource.KilnCatalog, e2, "")
                : new GameTextTable(lang, GameTextSource.None, null, $"{why}; no Kiln catalog English either: {Texts.Status.Reason}");
        }
        return GameTextStore.TryGet(lang) is { } t ? new GameTextTable(lang, GameTextSource.Game, t.Texts, t.Reason) : null;
    }

    /// <summary>
    /// Reads one language's texts (see <see cref="TryGetTexts"/> for <paramref name="language"/>) and completes with the
    /// table, on a worker: never on the caller's thread, never faults for missing data (an unavailable table says why).
    /// Once read, a table answers at once. Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    /// <exception cref="OperationCanceledException"><paramref name="cancellationToken"/> was cancelled.</exception>
    [AnyThread]
    public static async Task<GameTextTable> LoadTextsAsync(string? language, CancellationToken cancellationToken = default)
    {
        LookupCaps.Rows.Ensure();
        var lang = GameTextStore.Resolve(language);
        if (GameTextStore.Unavailable is { } why && OnServerWithCatalog())
        {
            if (!IsCatalogLanguage(lang)) return new GameTextTable(lang, GameTextSource.None, null, why);
            await Texts.Preload().WaitAsync(cancellationToken).ConfigureAwait(false);
            return Texts.Loaded is { } english ? new GameTextTable(lang, GameTextSource.KilnCatalog, english, "")
                : new GameTextTable(lang, GameTextSource.None, null, $"{why}; no Kiln catalog English either: {Texts.Status.Reason}");
        }
        var (texts, reason) = await GameTextStore.LoadAsync(lang, cancellationToken).ConfigureAwait(false);
        return new GameTextTable(lang, GameTextSource.Game, texts, reason);
    }

    /// <summary>
    /// The text of a loca tag id in <paramref name="language"/> with the game's markup, on any thread, without waiting:
    /// <see cref="TryGetTexts"/>'s table's text. Null for an unknown tag or an empty text, while the language's table is
    /// read (the first call starts that), and where it cannot be read (see <see cref="GameTextsCapability"/>). For the player's
    /// language on the game client's main thread <see cref="Text(uint)"/> also answers Kiln's new content.
    /// Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    [AnyThread]
    public static string? TextIn(uint locaId, string? language)
    {
        if (locaId == 0) return null;
        if (GameTextStore.Unavailable is not null && OnServerWithCatalog()) return TryGetTexts(language)?.Get(locaId);
        LookupCaps.Rows.Ensure();
        return GameTextStore.Text(locaId, GameTextStore.Resolve(language));   // no table object: a label drawn every frame
    }

    /// <summary>The text of a LocaTag reference (a GUID, hashed as the game does) in <paramref name="language"/>; see
    /// <see cref="TextIn(uint, string?)"/>. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    [AnyThread]
    public static string? TextIn(Guid locaTag, string? language) => locaTag == Guid.Empty ? null : TextIn(Texts.TagOf(locaTag), language);

    // A dedicated server (not a client without kiln.assets): the one place a Kiln catalog's English stands in.
    private static bool OnServerWithCatalog() => GameTextStore.Unavailable == GameTextStore.ServerReason;

    private static bool IsCatalogLanguage(string language) => language is "en-us" or "en";
}

/// <summary>The rows of the content lookups (game texts, voxel materials): managed, reported by Kiln.Content from what
/// kiln.assets and kiln.types offer, every 25 steps while they may change (docs/kiln/services/caps.md).</summary>
internal static class LookupCaps
{
    public const string AssetsResources = ResourceStore.ResourcesRow;
    public const string Types = "kiln.types";

    internal static readonly ServiceCapRows Rows = new("kiln.content lookups", Report, everySteps: 25);

    // The fallback reason when no needed row is off with its own: service-missing (kiln.assets' reads or kiln.types' game
    // types are not here), never Unknown, which ServiceCaps refuses to store (the row was left out before review 31.13).
    private static void Report()
    {
        var why = GameTextStore.Unavailable;
        if (why == GameTextStore.ServerReason)
            ServiceCaps.Report(Content.GameTextsCapability, CapabilityState.Unavailable, CapabilityReason.WrongSide, why);
        else
            ServiceCaps.Static(Content.GameTextsCapability, why is null, CapabilityReason.ServiceMissing, why ?? "", AssetsResources, Types);
        var materials = VoxelMaterialData.Unavailable;
        ServiceCaps.Static(Content.VoxelMaterialsCapability, materials is null, CapabilityReason.ServiceMissing, materials ?? "", AssetsResources, Types);
    }
}
