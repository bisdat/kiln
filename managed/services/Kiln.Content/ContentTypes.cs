// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>The game registries kiln.content exposes (Forge: <c>ForgeRegistries.ITEMS</c>, recipe manager, loot tables).</summary>
public enum ContentKind
{
    /// <summary>Every item definition (<c>keen::ItemInfo</c>): max stack, rarity, damage, armor, fuel, ...</summary>
    Items = 1,
    /// <summary>Every crafting recipe (<c>keen::RecipeInfo</c>): ingredients, results, station, duration, unlock.</summary>
    Recipes = 2,
    /// <summary>Random loot per item (<c>keen::LootableItemInfo</c>, containers and world pickups): stack sizes.</summary>
    Loot = 3,
    /// <summary>Default-inventory drop tables (enemy drops, fixed chests); filled as the game first uses each table.</summary>
    Drops = 4,
}

/// <summary>How a field's value is stored.</summary>
public enum ContentValueType
{
    /// <summary>No value.</summary>
    None = 0,
    /// <summary>A true or false value.</summary>
    Bool = 1,
    /// <summary>Integers, enums and flag sets.</summary>
    Int = 2,
    /// <summary>A floating-point number.</summary>
    Float = 3,
    /// <summary><c>keen::Time</c>, read and written in seconds.</summary>
    Time = 4,
    /// <summary>A GUID or a reference to another resource (read-only).</summary>
    Guid = 5,
}

/// <summary>Plain, enum or flag-set integer.</summary>
public enum ContentEnumKind
{
    /// <summary>A plain integer.</summary>
    Plain = 0,
    /// <summary>An enum: one of a list of named values.</summary>
    Enum = 1,
    /// <summary>A flag set: several named values together.</summary>
    Flags = 2,
}

/// <summary>One value read from a registry.</summary>
/// <param name="Name">The enum value's name (enums), the flag names joined by "," (flag sets), else null.</param>
/// <param name="Type">How the value is stored.</param>
/// <param name="Integer">The value of an <see cref="ContentValueType.Int"/> (integer, enum or flag set) or
/// <see cref="ContentValueType.Bool"/> field (0 or 1).</param>
/// <param name="Number">The value of a <see cref="ContentValueType.Float"/> field, or of a
/// <see cref="ContentValueType.Time"/> field in seconds.</param>
/// <param name="Guid">The value of a <see cref="ContentValueType.Guid"/> field.</param>
public readonly record struct ContentValue(ContentValueType Type, long Integer, double Number, Guid Guid, string? Name)
{
    /// <summary>The value as a boolean: true when <see cref="Integer"/> is not 0.</summary>
    public bool Bool => Integer != 0;

    /// <summary>The value as a double (integers, floats, seconds for times).</summary>
    public double AsDouble => Type is ContentValueType.Int or ContentValueType.Bool ? Integer : Number;

    /// <summary>The value as text: true or false, the enum or flag names, the number, seconds with an <c>s</c>, or
    /// the GUID.</summary>
    public override string ToString() => Type switch
    {
        ContentValueType.Bool => Bool ? "true" : "false",
        ContentValueType.Int => Name ?? Integer.ToString(),
        ContentValueType.Float => Number.ToString("G7", System.Globalization.CultureInfo.InvariantCulture),
        ContentValueType.Time => Number.ToString("G7", System.Globalization.CultureInfo.InvariantCulture) + "s",
        ContentValueType.Guid => Guid.ToString(),
        _ => "?",
    };
}

/// <summary>A field of a registry's struct, from the game's own reflection, as a dotted path
/// (<c>maxStackSize</c>, <c>damageSetup.dmgMod</c>, <c>perkIds[2]</c>).</summary>
/// <param name="Index">The field's index in the registry's field list.</param>
/// <param name="Path">The field's dotted path.</param>
/// <param name="TypeName">The game's type of the field, such as <c>keen::float32</c>.</param>
/// <param name="Offset">The field's offset in bytes from the start of the struct.</param>
/// <param name="Bytes">The field's size in bytes (1, 2, 4, 8 or 16).</param>
/// <param name="Type">How the field's value is stored.</param>
/// <param name="Signed">True for a signed integer.</param>
/// <param name="Writable">True when a mod can set the field.</param>
/// <param name="EnumKind">Whether the field is a plain integer, an enum or a flag set.</param>
/// <param name="Values">The names and numbers of an enum's or flag set's values.</param>
public sealed record ContentField(int Index, string Path, string TypeName, int Offset, int Bytes, ContentValueType Type,
    bool Signed, bool Writable, ContentEnumKind EnumKind, IReadOnlyList<(string Name, long Value)> Values)
{
    /// <summary>A reference to another resource (a 16-byte GUID typed by the game: a model, a template, a perk, ...):
    /// not writable as a value, but settable with <see cref="ContentEntry.SetRef(string, string, bool)"/> (kitbash).</summary>
    public bool IsReference { get; init; }

    /// <summary>The path and the type, with a note for a reference or a read-only field.</summary>
    public override string ToString() => $"{Path}: {TypeName}{(Writable ? "" : IsReference ? " (reference)" : " (read-only)")}";
}

/// <summary>One ingredient or result of a recipe. <see cref="Category"/> != 0: "any item of that input category".</summary>
/// <param name="Item">The item.</param>
/// <param name="Count">How many.</param>
/// <param name="Category">An input category id, or 0 for none. Not 0: any item of that category.</param>
public readonly record struct RecipeStack(ItemId Item, int Count, uint Category = 0)
{
    /// <summary>A stack of <paramref name="count"/> of the item named <paramref name="item"/> (a debug name, id or
    /// GUID). Throws <see cref="ArgumentException"/> for an item the registry does not know.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.content")]
    public static RecipeStack Of(string item, int count) =>
        new(Content.Items.Get(item) is { } e ? new ItemId(e.Id) : throw new ArgumentException($"unknown item {item}"), count);

    /// <summary>The item's name (or the category's id) and the count, for logs.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.content")]
    public override string ToString() =>
        Category != 0 ? $"category 0x{Category:x8} x{Count}" : $"{Content.Items.Get(Item.Value)?.Name ?? Item.ToString()} x{Count}";
}

/// <summary>One stack of a drop table (depth-first over the table's groups).</summary>
/// <param name="Chance">spawnProbability, 0..1.</param>
/// <param name="Weight">Weight inside a selector group (one of the group's stacks is picked by weight).</param>
/// <param name="Group">Index of the group the stack belongs to (read-only).</param>
/// <param name="Item">The item the stack drops.</param>
/// <param name="CountMin">The fewest the stack drops.</param>
/// <param name="CountMax">The most the stack drops.</param>
public readonly record struct DropStack(ItemId Item, int CountMin, int CountMax, float Chance, float Weight, int Group);

/// <summary>A conflict: two owners (mods / data packs) set the same field to different values; the later one wins.</summary>
/// <param name="Field">The field both set.</param>
/// <param name="Winner">The owner whose value is in effect.</param>
/// <param name="WinnerValue">The value the winner set.</param>
/// <param name="Loser">The owner whose value is overridden.</param>
/// <param name="LoserValue">The value the loser set.</param>
public sealed record ContentConflict(string Field, string Winner, string WinnerValue, string Loser, string LoserValue)
{
    /// <summary>The field, both owners and their values, and which one wins.</summary>
    public override string ToString() => $"{Field}: {Loser} sets {LoserValue}, {Winner} sets {WinnerValue} ({Winner} wins)";
}

/// <summary>What applying a data-pack document did.</summary>
/// <param name="Applied">How many changes were applied.</param>
/// <param name="Errors">What went wrong, one message each.</param>
/// <param name="Warnings">What was skipped or looked wrong, one message each.</param>
/// <param name="Conflicts">Fields that more than one owner set to different values.</param>
public sealed record ContentReport(int Applied, IReadOnlyList<string> Errors, IReadOnlyList<string> Warnings,
    IReadOnlyList<ContentConflict> Conflicts)
{
    /// <summary>True when there are no errors.</summary>
    public bool Ok => Errors.Count == 0;
}

/// <summary>State of kiln.content.</summary>
/// <param name="Ready">True when the registries are loaded and the data packs are applied.</param>
/// <param name="HasItems">True when the items registry works on this game build.</param>
/// <param name="HasRecipes">True when the recipes registry works on this game build.</param>
/// <param name="HasLoot">True when the loot registry works on this game build.</param>
/// <param name="HasDrops">True when the drops registry works on this game build.</param>
/// <param name="StackGuard">True when stacks above an item's maximum are kept instead of being cut down by the
/// game.</param>
/// <param name="Items">How many entries the items registry holds.</param>
/// <param name="Recipes">How many entries the recipes registry holds.</param>
/// <param name="Loot">How many entries the loot registry holds.</param>
/// <param name="Drops">How many entries the drops registry holds.</param>
/// <param name="Packs">How many data-pack files were applied.</param>
/// <param name="PackErrors">How many problems the last application of the packs had.</param>
/// <param name="Conflicts">How many fields more than one owner set to different values.</param>
/// <param name="Changed">How many fields currently differ from the game's original.</param>
/// <param name="Generation">How many times the registries were applied (+1 per world load or pack reload).</param>
/// <param name="Detail">A short text with more detail.</param>
public sealed record ContentStatus(bool Ready, bool HasItems, bool HasRecipes, bool HasLoot, bool HasDrops, bool StackGuard,
    int Items, int Recipes, int Loot, int Drops, int Packs, int PackErrors, int Conflicts, int Changed, ulong Generation,
    string Detail)
{
    // Counted when first read: that lists every definition, and Content.Ready and the ready checks ask for the status often.
    private readonly Lazy<(int Definitions, int Placeholders)>? _counts;
    private readonly int? _definitions, _placeholders;

    /// <summary>New items and recipes defined by mods and data packs (all states).</summary>
    public int Definitions { get => _definitions ?? _counts?.Value.Definitions ?? 0; init => _definitions = value; }

    /// <summary>Placeholders "[Missing: mod:item]" registered for items of mods that are gone (missing content).</summary>
    public int Placeholders { get => _placeholders ?? _counts?.Value.Placeholders ?? 0; init => _placeholders = value; }

    internal Func<(int Definitions, int Placeholders)> CountedBy { init => _counts = new(value); }
}

/// <summary>
/// The content registries are loaded and every data pack is applied (Forge: the moment after data packs load,
/// <c>TagsUpdatedEvent</c> / <c>ServerStartedEvent</c> for registries). Fires on the tick thread at world load
/// (before the save is read and before players can join) and again after a data-pack reload. Apply code changes in a
/// handler: they are layered over the data packs. Not cancelable; no fields to change.
/// </summary>
public sealed class ContentReadyEvent : KilnEvent
{
    internal ContentReadyEvent(ulong generation, bool reload)
    {
        Generation = generation;
        Reload = reload;
    }

    /// <summary>+1 per application (1 = world load).</summary>
    public ulong Generation { get; }

    /// <summary>True after a data-pack reload (a pack file changed, or a mod with packs was loaded / unloaded).</summary>
    public bool Reload { get; }
}
