// SPDX-License-Identifier: MIT
// Station menus with tabs of their own (chapter 16a; dev31 out/16a-station-tabs/DESIGN.md): what an author writes. A station
// with a workshop of its own lays out its crafting menu in tabs: in its block (MenuBlock.Tabs, shown from content ready on), or
// at run time as a layer of its own (StationMenus.Define). Recipes are named by their MenuKey and icons by "the n-th tab of the
// menu it is like": nothing names a game function, offset or reflection name. The record, its checks and the MenuKey binding
// table: StationMenuCompiler.cs; layers and their upkeep: StationMenuRuntime.cs; the store: Kiln.Content's StationMenuStore.
// docs/kiln/modding/world-blocks.md "Menus with tabs of their own".
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>A recipe in a station's menu, by its <see cref="RecipeBlock.MenuKey"/>: <c>"key"</c> for a recipe of the mod that defines the
/// station, <c>"other-mod:key"</c> for another mod's. It names the one recipe with that key that runs at the station's own workshop. A
/// recipe's output or name never stands in for its key.</summary>
[JsonConverter(typeof(StationRecipeRefConverter))]
public readonly record struct StationRecipeRef(string Value)
{
    /// <summary>A recipe by its key, or <c>"other-mod:key"</c>.</summary>
    public static implicit operator StationRecipeRef(string value) => new(value);

    /// <summary>The reference as written.</summary>
    public override string ToString() => Value;
}

/// <summary>A tab of a station's crafting menu: its name, the icon it borrows, and its sets of recipes, in this order.</summary>
public sealed record StationMenuTabBlock
{
    /// <summary>The tab's key: letters, digits, '.', '_' and '-', at most 64. Unique in the menu; its id follows from it, so keep it.</summary>
    public required string Key { get; init; }
    /// <summary>What the tab says: one text for every language, or one per language (at most 35, each 1 to 4096 bytes).</summary>
    [JsonConverter(typeof(StationMenuTextConverter))]
    public required UiText Name { get; init; }
    /// <summary>The icon of the n-th tab (0 the first) of the menu the station is like (<see cref="MenuBlock.Like"/>, else its kind's),
    /// as the game shipped it. -1: the first. At most 14.</summary>
    public int IconFromTab { get; init; } = -1;
    /// <summary>Its sets of recipes, 1 to 28 (a tab that lists nothing has one set with no recipes). The first set has no heading: the tab's name heads it.</summary>
    public required IReadOnlyList<StationMenuSetBlock> Sets { get; init; }
}

/// <summary>A set of recipes in a tab, under a heading of its own.</summary>
public sealed record StationMenuSetBlock
{
    /// <summary>The set's key: letters, digits, '.', '_' and '-', at most 64. Unique in its tab.</summary>
    public required string Key { get; init; }
    /// <summary>The heading over the set. Null for the first set of a tab, which must have none; optional for the others.</summary>
    [JsonConverter(typeof(StationMenuTextConverter))]
    public UiText? Heading { get; init; }
    /// <summary>The recipes it lists, in this order, by their <see cref="RecipeBlock.MenuKey"/>. Each one once in the menu, at most 512 in a
    /// tab. A recipe of the station that no set lists is still made there, but the menu does not show it.</summary>
    public required IReadOnlyList<StationRecipeRef> Recipes { get; init; }
}

/// <summary>A layout of a station's menu that a mod lays over it at run time: <see cref="StationMenus.Define"/>.</summary>
public sealed record StationMenuDefinition
{
    /// <summary>The layout's key in your mod: letters, digits, '.', '_' and '-', at most 64. The same key again replaces your layout.
    /// A station's own tabs (<see cref="MenuBlock.Tabs"/>) are <c>"&lt;station&gt;.menu"</c> of its mod.</summary>
    public required string Key { get; init; }
    /// <summary>The tabs, 1 to 15. A menu that lists nothing has one tab with one empty set.</summary>
    public required IReadOnlyList<StationMenuTabBlock> Tabs { get; init; }
}

/// <summary>A layout <see cref="StationMenus.Define"/> laid over a station's menu. Dispose takes it off again: the layout under it
/// shows again (the station's own tabs, another mod's layout, or the game's menu).</summary>
public sealed class StationMenuRegistration : IDisposable
{
    internal StationMenuRegistration(ulong owner, ulong handle, Guid definitionId)
    {
        Owner = owner;
        Handle = handle;
        DefinitionId = definitionId;
    }

    internal ulong Owner { get; }
    internal ulong Handle { get; }
    internal bool Disposed { get; set; }

    /// <summary>The layout's id: the same for your mod and the definition's key on every machine.</summary>
    public Guid DefinitionId { get; }

    /// <summary>The generation of this game's station menus now: it goes up by one with each change of any of them.</summary>
    public ulong Generation => StationMenuStore.Generation;

    /// <summary>Takes the layout off. Only the mod that defined it, on the tick thread; a second call does nothing.</summary>
    /// <exception cref="ArgumentException">Another mod (or an earlier load of this one) calls it.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    [TickThreadOnly]
    public void Dispose() => StationMenuRuntime.Dispose(this);
}

/// <summary>What <see cref="StationMenus.Define"/> answered.</summary>
public sealed record MenuDefinitionResult : IKilnResult
{
    /// <summary>The layout is stored and shows on the station's menu.</summary>
    public bool Ok { get; init; }
    /// <summary>Why not, a word first: <c>PendingContent</c> (a recipe it lists is not in the game yet: try again after the next
    /// content change), <c>ContentSyncDisabled</c>, <c>PeersLackStationMenus</c> (a joined Kiln player's game cannot show it),
    /// <c>NoHoldGate</c>, <c>Refused</c>. Empty when <see cref="Ok"/>.</summary>
    public string Reason { get; init; } = "";
    /// <summary>The layout, to take it off again. The same object when the same mod defines the same key again.</summary>
    public StationMenuRegistration? Registration { get; init; }
    /// <summary>The generation of this game's station menus after the call.</summary>
    public ulong Generation { get; init; }
}

/// <summary>Station menus with tabs of their own, laid out at run time. A station's own tabs go in its block (<see cref="MenuBlock.Tabs"/>).</summary>
public static class StationMenus
{
    /// <summary>
    /// Lays <paramref name="definition"/> over the menu of <paramref name="station"/>, as a layer of the calling mod. The layer defined
    /// last shows; disposing it shows the one under it again. A station with a workshop of its own only: a game workshop keeps the
    /// game's tabs. Each recipe it lists has to be there, once, at this station, with that <see cref="RecipeBlock.MenuKey"/>.
    /// The same key again replaces the layer (the same layout again changes nothing).
    /// </summary>
    /// <param name="station">A station of any loaded mod, as <see cref="Blocks.Define{T}"/> or <see cref="Blocks.Find"/> gave it.</param>
    /// <param name="definition">The layout.</param>
    /// <returns>Ok, or why not (<see cref="MenuDefinitionResult.Reason"/>): nothing changed then.</returns>
    /// <exception cref="ArgumentException">A bad key, count, heading or icon; a recipe it lists binds to none, to two, to one of another
    /// workshop, or is listed twice; a station without a workshop of its own.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or not from mod code.</exception>
    /// <exception cref="KilnUnavailableException"><c>stationmenus.store</c>: this Kiln has no store for station menus.</exception>
    [TickThreadOnly, RequiresCapability("stationmenus.store")]
    public static MenuDefinitionResult Define(BlockHandle<StationBlock> station, StationMenuDefinition definition) => StationMenuRuntime.Define(station, definition);
}

/// <summary>A <see cref="StationRecipeRef"/> in JSON: a string, <c>"key"</c> or <c>"mod:key"</c>, checked as it is read.</summary>
internal sealed class StationRecipeRefConverter : JsonConverter<StationRecipeRef>
{
    public override StationRecipeRef Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        if (r.TokenType != JsonTokenType.String) throw new JsonException("a recipe in a menu is a string: its menuKey, or \"other-mod:key\"");
        var s = r.GetString() ?? "";
        if (StationMenuCompiler.RefProblem(s) is { } problem) throw new JsonException(problem);
        return new(s);
    }

    public override void Write(Utf8JsonWriter w, StationRecipeRef v, JsonSerializerOptions o) => w.WriteStringValue(v.Value);
}

/// <summary>A menu's text in JSON: a string, or an object of language to text, as <c>UiBlockTextConverter</c> reads it; two keys that are
/// the same language once lower-cased (<c>"en"</c> and <c>"EN"</c>, <c>"pt_br"</c> and <c>"pt-BR"</c>) are refused, never one of them dropped.</summary>
internal sealed class StationMenuTextConverter : JsonConverter<UiText>
{
    public override UiText? Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        if (r.TokenType == JsonTokenType.String) return new UiText(r.GetString() ?? "");
        if (r.TokenType != JsonTokenType.StartObject) throw new JsonException("a text is a string, or an object of language to text: { \"en\": \"...\", \"de\": \"...\" }");
        var texts = new Dictionary<string, string>(StringComparer.Ordinal);
        var written = new Dictionary<string, string>(StringComparer.Ordinal);
        while (r.Read() && r.TokenType == JsonTokenType.PropertyName)
        {
            var language = r.GetString()!;
            r.Read();
            if (r.TokenType != JsonTokenType.String) throw new JsonException($"the text for \"{language}\" is a string");
            var key = StationMenuCompiler.LanguageKey(language);
            if (written.TryGetValue(key, out var other))
                throw new JsonException($"\"{other}\" and \"{language}\" are the same language ({key}): give it once");
            written[key] = language;
            texts[key] = r.GetString()!;
        }
        if (texts.Count == 0) throw new JsonException("a text in languages needs at least one: { \"en\": \"...\" }");
        return new UiText(texts);
    }

    public override void Write(Utf8JsonWriter w, UiText v, JsonSerializerOptions o)
    {
        if (v.Languages.Count == 1 && v.Languages.First() == "*")
        {
            w.WriteStringValue(v.For("*"));
            return;
        }
        w.WriteStartObject();
        foreach (var language in v.Languages.Order(StringComparer.Ordinal)) w.WriteString(language, v.For(language));
        w.WriteEndObject();
    }
}
