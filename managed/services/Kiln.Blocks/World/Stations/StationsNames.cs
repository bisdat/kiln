// SPDX-License-Identifier: MIT
// Names for players of stations and recipes (0.2.0-dev.29.15): StationInfo.DisplayName, StationRecipe.DisplayName, and
// Stations.WorkshopName / RecipeName, which also work where kiln.stations does not (a joined client, the main menu): they read
// the game's own data (Kiln.Items' GameNames: WorkshopRegistryResource, RecipeRegistryResource, the game's texts).
namespace Kiln;

public static partial class Stations
{
    /// <summary>
    /// A workshop's name for players (since 0.2.0-dev.29.15): the name of the item that places its station ("Charcoal Kiln"),
    /// else the workshop's own name text, in the game's text language on a game client (the game's own texts; also on a
    /// client that joined another's world, where the rest of Stations is not there: name the ids your server half sends). On
    /// a dedicated server, whose data has no texts, the English names Kiln has (Kiln's shipped item names; the catalog's
    /// texts when one is built). Null for a workshop the game's data does not have (a mod's own workshop), and until the
    /// game's names are read (the first call starts that in the background; a fraction of a second).
    /// </summary>
    /// <remarks>Any thread; never waits.</remarks>
    [AnyThread]
    public static string? WorkshopName(uint workshop)
    {
        if (workshop == 0) return null;
        GameNames.Preload();
        if (GameNames.Workshop(workshop) is not { } w) return null;
        return Items.DisplayName(w.Item) is { Length: > 0 } item ? item : GameNames.Text(w.NameTag) ?? EnglishText(w.NameTag);
    }

    /// <summary>
    /// A recipe's name for players (since 0.2.0-dev.29.15): its first output's name ("Charcoal"), else the recipe's own name
    /// text, in the game's text language on a game client, as <see cref="WorkshopName"/>. Null for a recipe the game's data does
    /// not list (a new recipe of kiln.content: <see cref="StationRecipe.DisplayName"/> names it from its outputs on the
    /// server), and until the game's names are read.
    /// </summary>
    /// <remarks>Any thread; never waits.</remarks>
    [AnyThread]
    public static string? RecipeName(uint recipe)
    {
        if (recipe == 0) return null;
        GameNames.Preload();
        if (GameNames.Recipe(recipe) is not { } r) return null;
        if (r.FirstOutput.Value != 0 && Items.DisplayName(r.FirstOutput) is { Length: > 0 } item) return item;
        return GameNames.Text(r.NameTag) ?? EnglishText(r.NameTag);
    }

    // a dedicated server has no texts of the game's: Kiln.Content's Texts (the catalog's en-us table, when one is built)
    private static string? EnglishText(uint tag)
    {
        if (tag == 0) return null;
        try
        {
            var loaded = Texts.Preload();  // read in the background the first time: no wait here
            return loaded.IsCompletedSuccessfully && loaded.Result ? Texts.GetPlain(tag) : null;
        }
        catch (Exception e) when (e is InvalidOperationException or IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }
}
