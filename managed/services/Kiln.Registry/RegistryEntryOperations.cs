// SPDX-License-Identifier: MIT
using System.Diagnostics.CodeAnalysis;
namespace Kiln.Registry;

/// <summary>Entry creation and visibility, available only on the registries that support them.
/// GameStart intent is retained for each world; later entry points return RestartRequired.</summary>
[Experimental("KILNX031"), RegistryThreadOnly]
public static class RegistryEntryOperations
{
    /// <summary>Adds a new item under the current mod's stable id, inheriting a stable entry's shape and values.
    /// Configure typed fields here; Kiln translates identifiers and never deletes existing saved copies.</summary>
    [RequiresCapability("registry.items.add")]
    public static void AddLike(this RegistryEntries<ItemEntry> entries, RegistryId id, RegistryId like, Action<ItemEntry> configure)
    {
        ArgumentNullException.ThrowIfNull(entries); entries.AddLikeCore(id, like, configure);
    }
    /// <summary>Adds a recipe under the current mod's stable id, inheriting the source recipe's station and requirements.</summary>
    [RequiresCapability("registry.recipes.add")]
    public static void AddLike(this RegistryEntries<RecipeEntry> entries, RegistryId id, RegistryId like, Action<RecipeEntry> configure)
    {
        ArgumentNullException.ThrowIfNull(entries); entries.AddLikeCore(id, like, configure);
    }
    /// <summary>Hides matching items from loot, drops and recipes. Copies already in inventories, equipment or containers stay usable.
    /// Refused whole for an item when drop exclusion is unproven or would leave an unsafe all-zero group.</summary>
    [RequiresCapability("registry.items.remove")]
    public static void Remove(this RegistryEntries<ItemEntry> entries, string pattern)
    {
        ArgumentNullException.ThrowIfNull(entries); entries.RemoveCore(pattern);
    }
    /// <summary>Hides matching recipes from station lists and the crafting menu; recipe data is retained.</summary>
    [RequiresCapability("registry.recipes.remove")]
    public static void Remove(this RegistryEntries<RecipeEntry> entries, string pattern)
    {
        ArgumentNullException.ThrowIfNull(entries); entries.RemoveCore(pattern);
    }
    /// <summary>Disables matching random-loot entries without removing their items or existing copies.</summary>
    [RequiresCapability("registry.loot.remove")]
    public static void Remove(this RegistryEntries<ItemLootEntry> entries, string pattern)
    {
        ArgumentNullException.ThrowIfNull(entries); entries.RemoveCore(pattern);
    }
}
