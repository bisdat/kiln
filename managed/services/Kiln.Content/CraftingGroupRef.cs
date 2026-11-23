// SPDX-License-Identifier: MIT
// A place in the game's crafting menu that is already there (since 0.2.0-dev.30.5): a recipe defined with RecipeDefinition.MenuGroup is
// listed in that tab (and subcategory) of that workshop's menu, instead of the place kiln.content's rules pick
// (native/services/content/newcontent.cpp, menu_sync). The menu's layout: docs/re/game-ui.md "Crafting menus".
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>A tab of a workshop's crafting menu that the game already has, and in it one list (subcategory): where a recipe is listed. The
/// tabs are <see cref="CraftingGroups"/> constants (<c>CraftingGroups.Carpenter.Essentials</c>); as text <c>"0x&lt;workshop&gt;/&lt;tab GUID&gt;"</c>, with
/// <c>"/&lt;subcategory GUID&gt;"</c> for a list other than the tab's first. Since 0.2.0-dev.30.5 (mod API 0.2.14).</summary>
[JsonConverter(typeof(CraftingGroupRefConverter))]
public readonly record struct CraftingGroupRef
{
    /// <summary>A tab, and a subcategory in it (<see cref="Guid.Empty"/>: the tab's first list, the one without a header).</summary>
    /// <param name="workshop">The workshop whose menu it is (0: the player's own crafting menu).</param>
    /// <param name="group">The tab's GUID (the game's <c>FbUiCraftingRecipeGroup.groupId</c>).</param>
    /// <param name="set">The subcategory's GUID (<c>FbUiCraftingRecipeSet.recipeSetId</c>).</param>
    /// <exception cref="ArgumentException"><paramref name="group"/> is empty.</exception>
    public CraftingGroupRef(uint workshop, Guid group, Guid set = default)
    {
        if (group == Guid.Empty) throw new ArgumentException("a tab has a GUID", nameof(group));
        (Workshop, Group, Set) = (workshop, group, set);
    }

    /// <summary>The workshop whose crafting menu it is (0: the player's own).</summary>
    public uint Workshop { get; }

    /// <summary>The tab's GUID.</summary>
    public Guid Group { get; }

    /// <summary>The subcategory's GUID; <see cref="Guid.Empty"/>: the tab's first list.</summary>
    public Guid Set { get; }

    /// <summary>The text form: <c>"0x&lt;workshop&gt;/&lt;tab&gt;"</c> or <c>"0x&lt;workshop&gt;/&lt;tab&gt;/&lt;subcategory&gt;"</c>.</summary>
    public override string ToString() =>
        Set == Guid.Empty ? $"0x{Workshop:x8}/{Group:D}" : $"0x{Workshop:x8}/{Group:D}/{Set:D}";

    /// <summary>Reads the text form.</summary>
    /// <exception cref="FormatException">It is not <c>"0x&lt;workshop&gt;/&lt;tab GUID&gt;[/&lt;subcategory GUID&gt;]"</c>.</exception>
    public static CraftingGroupRef Parse(string text) =>
        TryParse(text, out var r) ? r : throw new FormatException($"\"{text}\" is not a crafting menu tab: \"0x<workshop>/<tab GUID>\" (CraftingGroups), or \"0x<workshop>/<tab GUID>/<subcategory GUID>\"");

    /// <summary>Reads the text form; false when it is not one.</summary>
    public static bool TryParse(string? text, out CraftingGroupRef result)
    {
        result = default;
        var parts = text?.Split('/') ?? [];
        if (parts.Length is < 2 or > 3 || !parts[0].StartsWith("0x", StringComparison.OrdinalIgnoreCase) || parts[0].Length is < 3 or > 10
            || !uint.TryParse(parts[0].AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var workshop)
            || !Guid.TryParseExact(parts[1], "D", out var group) || group == Guid.Empty)
            return false;
        var set = Guid.Empty;
        if (parts.Length == 3 && !Guid.TryParseExact(parts[2], "D", out set)) return false;
        result = new CraftingGroupRef(workshop, group, set);
        return true;
    }

    /// <summary>The text form, as <see cref="CraftingGroups"/> constants are.</summary>
    /// <exception cref="FormatException">It is not one.</exception>
    public static implicit operator CraftingGroupRef(string text) => Parse(text);
}

internal sealed class CraftingGroupRefConverter : JsonConverter<CraftingGroupRef>
{
    public override CraftingGroupRef Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) =>
        r.TokenType == JsonTokenType.String && CraftingGroupRef.TryParse(r.GetString(), out var g) ? g
            : throw new JsonException("a crafting menu tab is \"0x<workshop>/<tab GUID>\" or \"0x<workshop>/<tab GUID>/<subcategory GUID>\"");

    public override void Write(Utf8JsonWriter w, CraftingGroupRef v, JsonSerializerOptions o) => w.WriteStringValue(v.ToString());
}
