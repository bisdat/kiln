// SPDX-License-Identifier: MIT
// Typed gear values of an item type (Content.Gear): damage, armour, levels, rarity, ... in the numbers the game's own tooltip shows,
// written as item fields of the calling mod (a layer over the game's value, reverted with the mod, like every ContentEntry write).
// The same shape as a data pack's "gear" section (docs/kiln/services/content.md "Gear values"; native/services/content/gear.h).
namespace Kiln;

/// <summary>A kind of damage a weapon's hit is shared out in (<see cref="GearStats.Damage"/>), or one of the whole numbers the
/// game keeps per kind for what a tool harvests and breaks (wood, stone, metal, explosion).</summary>
public enum GearDamageKind
{
    /// <summary>Cut damage (<c>physicalCutDamage</c>).</summary>
    Cut,
    /// <summary>Pierce damage (<c>physicalPierceDamage</c>).</summary>
    Pierce,
    /// <summary>Blunt damage (<c>physicalBluntDamage</c>).</summary>
    Blunt,
    /// <summary>Fire damage (<c>magicalFireDamage</c>).</summary>
    Fire,
    /// <summary>Ice damage (<c>magicalIceDamage</c>).</summary>
    Ice,
    /// <summary>The game's fog damage.</summary>
    Shroud,
    /// <summary>Lightning damage (<c>magicalLightningDamage</c>).</summary>
    Lightning,
    /// <summary>Poison damage (<c>poisonDamage</c>).</summary>
    Poison,
    /// <summary>Healing (<c>healing</c>).</summary>
    Healing,
    /// <summary>The whole number the game keeps for wood, not a share (<c>woodDamage</c>).</summary>
    Wood,
    /// <summary>The whole number the game keeps for stone, not a share (<c>stoneDamage</c>).</summary>
    Stone,
    /// <summary>The whole number the game keeps for metal, not a share (<c>metalDamage</c>).</summary>
    Metal,
    /// <summary>The whole number the game keeps for explosions, not a share (<c>explosionDamage</c>).</summary>
    Explosion,
}

/// <summary>A kind of armour an item gives (<see cref="GearStats.Armor"/>).</summary>
public enum GearArmorKind
{
    /// <summary>Physical armour (<c>physical</c>).</summary>
    Physical,
    /// <summary>Magical armour (<c>magical</c>).</summary>
    Magical,
    /// <summary>Armour against cut damage (<c>cut</c>).</summary>
    Cut,
    /// <summary>Armour against pierce damage (<c>pierce</c>).</summary>
    Pierce,
    /// <summary>Armour against blunt damage (<c>blunt</c>).</summary>
    Blunt,
    /// <summary>Armour against fire damage (<c>fire</c>).</summary>
    Fire,
    /// <summary>Armour against ice damage (<c>ice</c>).</summary>
    Ice,
    /// <summary>The game's fog armour.</summary>
    Shroud,
    /// <summary>Armour against lightning damage (<c>lightning</c>).</summary>
    Lightning,
}

/// <summary>What <see cref="GearStats"/> reads and writes: one item's fields (a <see cref="ContentEntry"/>, or a fake in tests).</summary>
internal interface IGearFields
{
    string Name { get; }
    /// <summary>The value now; null when this build's item has no such field.</summary>
    double? Current(string path);
    /// <summary>The game's original value; null when there is no such field.</summary>
    double? Original(string path);
    void Set(string path, double value);
    void SetInt(string path, long value);
    void SetBool(string path, bool value);
    void SetName(string path, string name);
    bool Reset(string path);
}

/// <summary>
/// The gear values of one item type, in the game's tooltip numbers (from <see cref="Content.Gear(string)"/>). Reads give the
/// values as they are now (every mod's layers applied); writes are layers of the calling mod over the game's value, reverted when
/// the mod unloads or reloads, and listed as conflicts when another mod or a data pack sets the same field. A change applies to
/// copies the game makes or reads afterwards; Kiln clients get it through the content sync, a player without Kiln sees the game's
/// numbers in the tooltip. Tick thread, as every <see cref="ContentEntry"/> call.
/// </summary>
public sealed class GearStats
{
    private const string MinLevelPath = "itemLevelRange.minLevel", MaxLevelPath = "itemLevelRange.maxLevel";
    private const string DmgModPath = "damageSetup.dmgMod", SpeedPath = "damageSetup.speedMod", BlockPath = "blockSetup.quality";

    private readonly IGearFields _f;

    internal GearStats(IGearFields fields) => _f = fields;

    /// <summary>The item's debug name.</summary>
    public string Item => _f.Name;

    /// <summary>The item deals damage of its own (<c>damageSetup.isSet</c>): weapons, wands, ammunition, tools.</summary>
    public bool HasDamage => (_f.Current("damageSetup.isSet") ?? 1) != 0;

    /// <summary>The item has armour values (<c>armorSetup.isSet</c>): armour, shields, some rings.</summary>
    public bool HasArmor => (_f.Current("armorSetup.isSet") ?? 1) != 0;

    /// <summary>The lowest level a copy is made at (<c>itemLevelRange.minLevel</c>): the level the recipe tooltip shows, and
    /// the one <see cref="Power"/> and <see cref="Armor"/> are read and solved at.</summary>
    public int Level => Math.Clamp((int)(_f.Current(MinLevelPath) ?? GearMath.MinLevel), GearMath.MinLevel, 255);

    /// <summary>The highest level a copy can roll (<c>itemLevelRange.maxLevel</c>); the same as <see cref="Level"/> for crafted gear.</summary>
    public int MaxLevel => (int)(_f.Current(MaxLevelPath) ?? Level);

    /// <summary>Sets the levels a copy is made at (1..55, lowest first; one level when <paramref name="highest"/> is left out).
    /// <see cref="Power"/> and the armour then follow the game's curve to the new level: set them afterwards to keep a number.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A level outside 1..55, or the highest below the lowest.</exception>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public void SetLevels(int lowest, int? highest = null)
    {
        var hi = highest ?? lowest;
        if (lowest < GearMath.MinLevel || hi > GearMath.MaxLevel || lowest > hi)
            throw new ArgumentOutOfRangeException(nameof(lowest), $"{Item}: levels {lowest}..{hi} are not within the game's item levels 1..55, lowest first");
        _f.SetInt(MinLevelPath, lowest);
        _f.SetInt(MaxLevelPath, hi);
    }

    /// <summary>The damage the tooltip shows at <see cref="Level"/> (0 for an item without damage). Set: solved into
    /// <c>damageSetup.dmgMod</c> at <see cref="Level"/> and <see cref="Speed"/>, so the tooltip shows this number.</summary>
    /// <exception cref="InvalidOperationException">Set on an item without damage of its own (<see cref="HasDamage"/>).</exception>
    /// <exception cref="ArgumentOutOfRangeException">Set outside 0..100000.</exception>
    public int Power
    {
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        get => HasDamage ? PowerAt(Level) : 0;
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        set
        {
            RequireDamage(nameof(Power));
            if (value is < 0 or > 100000) throw new ArgumentOutOfRangeException(nameof(value), $"{Item}: power {value} is outside 0..100000");
            _f.Set(DmgModPath, GearMath.DmgModFor(value, Level, Speed));
        }
    }

    /// <summary>The damage the tooltip shows for a copy at <paramref name="level"/> (the game's curve over the item's dmgMod and speed).</summary>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public int PowerAt(int level) => GearMath.PowerOf(_f.Current(DmgModPath) ?? 1, Speed, level);

    /// <summary>The damage factor of the weapon's speed class (<c>damageSetup.speedMod</c>); it multiplies <see cref="Power"/>.
    /// The attack speed itself is the weapon's animation. Set alone, the tooltip's damage changes with it: set <see cref="Power"/>
    /// afterwards to keep the number.</summary>
    /// <exception cref="ArgumentOutOfRangeException">Set at 0 or less, or above 100.</exception>
    public double Speed
    {
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        get => _f.Current(SpeedPath) ?? 1;
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        set
        {
            RequireDamage(nameof(Speed));
            if (!(value > 0 && value <= 100)) throw new ArgumentOutOfRangeException(nameof(value), $"{Item}: speed {value} is outside 0..100 (more than 0)");
            _f.Set(SpeedPath, value);
        }
    }

    /// <summary>A kind's share of the damage (0.5 = half of <see cref="Power"/>), or for wood, stone, metal and explosion the whole
    /// number the game keeps for what the item harvests or breaks.</summary>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public double Damage(GearDamageKind kind) => _f.Current(DamagePath(kind)) ?? 0;

    /// <summary>Sets a kind's share of the damage (0..10), or the whole number of wood, stone, metal or explosion (0..1000000).
    /// The game's own weapons have shares that add up to 1.</summary>
    /// <exception cref="InvalidOperationException">An item without damage of its own.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A value outside the kind's range.</exception>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public void SetDamage(GearDamageKind kind, double value)
    {
        RequireDamage(nameof(SetDamage));
        var whole = kind is GearDamageKind.Wood or GearDamageKind.Stone or GearDamageKind.Metal or GearDamageKind.Explosion;
        if (value < 0 || value > (whole ? 1000000 : 10) || double.IsNaN(value))
            throw new ArgumentOutOfRangeException(nameof(value), $"{Item}: damage {kind} {value} is outside 0..{(whole ? 1000000 : 10)}");
        if (whole) _f.SetInt(DamagePath(kind), (long)Math.Round(value));
        else _f.Set(DamagePath(kind), value);
    }

    /// <summary>The armour of a kind the tooltip shows at <see cref="Level"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public int Armor(GearArmorKind kind) => GearMath.ArmorOf(_f.Current(ArmorPath(kind)) ?? 0, Level);

    /// <summary>Sets the armour of a kind the tooltip shows at <see cref="Level"/> (0..100000), solved into the item's
    /// <c>armorSetup.distribution</c>.</summary>
    /// <exception cref="InvalidOperationException">An item without armour values (<see cref="HasArmor"/>).</exception>
    /// <exception cref="ArgumentOutOfRangeException">A value outside 0..100000.</exception>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public void SetArmor(GearArmorKind kind, int armor)
    {
        if (!HasArmor) throw new InvalidOperationException($"{Item} has no armour values (armorSetup.isSet is off)");
        if (armor is < 0 or > 100000) throw new ArgumentOutOfRangeException(nameof(armor), $"{Item}: armour {kind} {armor} is outside 0..100000");
        _f.Set(ArmorPath(kind), GearMath.ArmorShare(armor, Level));
    }

    /// <summary>How well the item blocks (<c>blockSetup.quality</c>, 0..1).</summary>
    /// <exception cref="ArgumentOutOfRangeException">Set outside 0..1.</exception>
    public double Block
    {
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        get => _f.Current(BlockPath) ?? 0;
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        set
        {
            if (value is < 0 or > 1 || double.IsNaN(value)) throw new ArgumentOutOfRangeException(nameof(value), $"{Item}: block {value} is outside 0..1");
            _f.Set(BlockPath, value);
        }
    }

    /// <summary>The item type's rarity (<c>rarity</c>): what the recipe tooltip shows and a copy is made with when the game does
    /// not roll it (<see cref="Rolled"/>). A copy has rarity + 1 enhancement slots, filled from the type's perk slots: a slot left
    /// empty makes the copy one rarity lower. Copies already made keep theirs.</summary>
    public ItemRarity Rarity
    {
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        get => (ItemRarity)Math.Clamp((int)(_f.Current("rarity") ?? 0), 0, 4);
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        set
        {
            if (!Enum.IsDefined(value)) throw new ArgumentOutOfRangeException(nameof(value), $"{Item}: {value} is not a rarity of the game's");
            _f.SetName("rarity", value.ToString());
        }
    }

    /// <summary>The game rolls the rarity of each new copy (<c>generateRarity</c>), as for loot weapons; false: every copy is
    /// <see cref="Rarity"/>.</summary>
    public bool Rolled
    {
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        get => (_f.Current("generateRarity") ?? 0) != 0;
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        set => _f.SetBool("generateRarity", value);
    }

    /// <summary>New copies get a gem socket (<c>gemSlotOverride</c>); on gear other than weapons this needs Kiln's sockets
    /// (docs/kiln/services/gems-sockets.md).</summary>
    public bool Socket
    {
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        get => (_f.Current("gemSlotOverride") ?? 0) != 0;
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        set => _f.SetBool("gemSlotOverride", value);
    }

    /// <summary>The max stack size (<c>maxStackSize</c>, 1..65535).</summary>
    /// <exception cref="ArgumentOutOfRangeException">Set outside 1..65535.</exception>
    public int MaxStack
    {
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        get => (int)(_f.Current("maxStackSize") ?? 1);
        [TickThreadOnly]
        [RequiresCapability("content.gear")]
        set
        {
            if (value is < 1 or > 65535) throw new ArgumentOutOfRangeException(nameof(value), $"{Item}: max stack {value} is outside 1..65535");
            _f.SetInt("maxStackSize", value);
        }
    }

    /// <summary>Takes the calling mod's layers off every field these values write (the game's values, or other mods', come
    /// back). Returns how many fields had a layer of this mod.</summary>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public int Reset()
    {
        var n = 0;
        foreach (var p in Paths) if (_f.Current(p) != null && _f.Reset(p)) n++;
        return n;
    }

    /// <inheritdoc/>
    public override string ToString() => $"gear {Item}";

    private void RequireDamage(string what)
    {
        if (!HasDamage) throw new InvalidOperationException($"{Item}: {what}: this item deals no damage of its own (damageSetup.isSet is off)");
    }

    internal static string DamagePath(GearDamageKind kind) => "damageSetup.distribution." + kind switch
    {
        GearDamageKind.Cut => "physicalCutDamage",
        GearDamageKind.Pierce => "physicalPierceDamage",
        GearDamageKind.Blunt => "physicalBluntDamage",
        GearDamageKind.Fire => "magicalFireDamage",
        GearDamageKind.Ice => "magicalIceDamage",
        GearDamageKind.Shroud => "magicalFogDamage",
        GearDamageKind.Lightning => "magicalLightningDamage",
        GearDamageKind.Poison => "poisonDamage",
        GearDamageKind.Healing => "healing",
        GearDamageKind.Wood => "woodDamage",
        GearDamageKind.Stone => "stoneDamage",
        GearDamageKind.Metal => "metalDamage",
        GearDamageKind.Explosion => "explosionDamage",
        _ => throw new ArgumentOutOfRangeException(nameof(kind)),
    };

    internal static string ArmorPath(GearArmorKind kind) => "armorSetup.distribution." + kind switch
    {
        GearArmorKind.Physical => "physical",
        GearArmorKind.Magical => "magical",
        GearArmorKind.Cut => "cut",
        GearArmorKind.Pierce => "pierce",
        GearArmorKind.Blunt => "blunt",
        GearArmorKind.Fire => "fire",
        GearArmorKind.Ice => "ice",
        GearArmorKind.Shroud => "fog",
        GearArmorKind.Lightning => "lightning",
        _ => throw new ArgumentOutOfRangeException(nameof(kind)),
    };

    private static readonly string[] Paths =
    [
        MinLevelPath, MaxLevelPath, DmgModPath, SpeedPath, BlockPath, "rarity", "generateRarity", "gemSlotOverride", "maxStackSize",
        .. Enum.GetValues<GearDamageKind>().Select(DamagePath),
        .. Enum.GetValues<GearArmorKind>().Select(ArmorPath),
    ];
}

/// <summary>A <see cref="ContentEntry"/> of the items registry as <see cref="GearStats"/> reads and writes it.</summary>
internal sealed class EntryGearFields(ContentEntry entry) : IGearFields
{
    public string Name => entry.Name;
    public double? Current(string path) => Content.Items.Field(path) != null ? entry.Get(path).AsDouble : null;
    public double? Original(string path) => Content.Items.Field(path) != null ? entry.Original(path).AsDouble : null;
    public void Set(string path, double value) => entry.Set(path, value);
    public void SetInt(string path, long value) => entry.Set(path, value);
    public void SetBool(string path, bool value) => entry.Set(path, value);
    public void SetName(string path, string name) => entry.Set(path, name);
    public bool Reset(string path) => entry.Reset(path);
}

public static partial class Content
{
    /// <summary>The capability of <see cref="Gear(string)"/> and the data packs' "gear" section.</summary>
    public const string GearCapability = "content.gear";

    /// <summary>The typed gear values of an item type (a debug name, <c>"0x…"</c> id or GUID), in the game's tooltip numbers;
    /// null when there is no such item. The same values a data pack's <c>"gear"</c> section sets
    /// (docs/kiln/services/content.md "Gear values").</summary>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public static GearStats? Gear(string item)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(item);
        return Items.Get(item) is { } e ? Gear(e) : null;
    }

    /// <summary>The typed gear values of an item type; null when there is no such item.</summary>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public static GearStats? Gear(ItemId item) => Items.Get(item) is { } e ? Gear(e) : null;

    /// <summary>The typed gear values of an entry of <see cref="Items"/>.</summary>
    /// <exception cref="ArgumentException">The entry is not an item.</exception>
    [TickThreadOnly]
    [RequiresCapability("content.gear")]
    public static GearStats Gear(ContentEntry item)
    {
        ArgumentNullException.ThrowIfNull(item);
        if (item.Kind != ContentKind.Items) throw new ArgumentException($"{item} is not an item", nameof(item));
        return new GearStats(new EntryGearFields(item));
    }
}
