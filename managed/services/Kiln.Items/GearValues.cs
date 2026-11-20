// SPDX-License-Identifier: MIT
// One item's values beyond its game components (the equipment wave, package P7): a multiplier of its power (the damage it
// deals) and of its armour, kept by kiln.items v8's scale table and applied where the game computes them (kiln_items.h
// "power and armour scales", docs/re/items.md section 20), and the values the game does not keep through a load (a level
// outside the type's range, a max durability), which Kiln keeps with the item as item data and writes again after each
// load. The keeping is Kiln.Blocks' (GearKeepRuntime, the item data key "kiln-gear:values", synced to players near the
// item so a Kiln client's tooltip can show the scaled numbers); Kiln.Items only asks it through GearKeep.
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>What <see cref="Items.SetPower"/> and <see cref="Items.SetArmor"/> did.</summary>
public enum GearValuesStatus
{
    /// <summary>Set (also when nothing changed).</summary>
    Done = 0,

    /// <summary>The item is gone, or its id names another item now: nothing changed.</summary>
    Stale = 1,

    /// <summary>8192 items have a scale already in this world: nothing changed.</summary>
    Full = 2,
}

/// <summary>One item's values beyond its game components (<see cref="Items.ValuesOf"/>).</summary>
/// <param name="Item">The item.</param>
/// <param name="Power">The multiplier of its power (1: its own). The damage of its swings, shots and casts follows it on
/// the server.</param>
/// <param name="Armor">The multiplier of the armour it adds to its wearer (1: its own).</param>
/// <param name="KeptBaseLevel">A base level outside its type's range that Kiln keeps and writes again after each load
/// (<see cref="GearWriteOptions.AnyLevel"/>); null: none.</param>
/// <param name="KeptDurabilityMax">A max durability Kiln keeps and writes again after each load
/// (<see cref="Items.SetDurabilityMax"/>); null: none.</param>
/// <param name="Kept">The values above are kept with the item through saves and restarts (Kiln's item data is on). When
/// false they last until the server world closes.</param>
public sealed record GearValues(ItemInstanceRef Item, double Power, double Armor, int? KeptBaseLevel, int? KeptDurabilityMax, bool Kept)
{
    /// <summary>One line for a log.</summary>
    public override string ToString() =>
        $"{Item}: power x{Power:0.###} armour x{Armor:0.###}" + (KeptBaseLevel is { } b ? $" base level {b}" : "") +
        (KeptDurabilityMax is { } d ? $" max durability {d}" : "") + (Kept ? " (kept)" : "");
}

/// <summary>What <see cref="Items.SetPower"/> or <see cref="Items.SetArmor"/> did. <see cref="Ok"/> when the value is
/// set; otherwise <see cref="Reason"/> says why and nothing changed.</summary>
/// <param name="Status">The outcome.</param>
/// <param name="Item">The item.</param>
/// <param name="Power">Its power multiplier now.</param>
/// <param name="Armor">Its armour multiplier now.</param>
/// <param name="Kept">Kiln keeps the multipliers with the item through saves and restarts (item data). False: they last
/// until the server world closes (Kiln's item data is off).</param>
/// <param name="Message">Why, for anything but <see cref="GearValuesStatus.Done"/>.</param>
public sealed record GearValuesResult(GearValuesStatus Status, ItemInstanceRef Item, double Power, double Armor, bool Kept, string Message) : IKilnResult
{
    /// <summary>True when the value is set.</summary>
    public bool Ok => Status == GearValuesStatus.Done;

    /// <summary>Why it did not happen; null when <see cref="Ok"/>.</summary>
    public string? Reason => Ok ? null : Message.Length > 0 ? Message : Status.ToString();

    /// <summary>The outcome in one line, for a log.</summary>
    public override string ToString() =>
        $"values {Status} {Item}: power x{Power:0.###} armour x{Armor:0.###}" + (Kept ? " (kept)" : "") + (Message.Length > 0 ? $": {Message}" : "");
}

public static unsafe partial class Items
{
    /// <summary>The lowest power or armour multiplier.</summary>
    public const double MinScale = 0.01;

    /// <summary>The highest power or armour multiplier.</summary>
    public const double MaxScale = 100;

    /// <summary>True when <see cref="SetPower"/> can act (capability <c>items.gear.power</c>).</summary>
    public static bool PowerAvailable => Can(Caps.GearPower) && V8Scales != null;

    /// <summary>True when <see cref="SetArmor"/> can act (capability <c>items.gear.armor</c>).</summary>
    public static bool ArmorAvailable => Can(Caps.GearArmor) && V8Scales != null;

    // the v8 table with the scales (package P7), or null
    internal static ItemsNative.TableV8* V8Scales
    {
        get
        {
            var t = V8;
            return t != null && ((ItemsNative.Table*)t)->Header.Size >= ItemsNative.V8ScaleEnd && t->GearScaleSet != null ? t : null;
        }
    }

    /// <summary>
    /// Multiplies one item's power: the damage of its swings, shots and casts (the game computes an item's power when an
    /// impact of it starts and multiplies the impact's damage by it; armour, block, crit and attributes act after, as for the
    /// game's own numbers). Only this item: another of its type is unchanged, and the item keeps it wherever it goes, through
    /// saves and restarts. 1 gives it its own power back. Server-authoritative: no player's game takes part.
    /// </summary>
    /// <param name="item">The item: <see cref="Held"/>, <see cref="Equipped"/>, a slot's <see cref="InventorySlot.Ref"/>,
    /// a give result's <see cref="ItemChange.Ref"/>.</param>
    /// <param name="scale"><see cref="MinScale"/> to <see cref="MaxScale"/>; 1 = its own power.</param>
    /// <returns>Set at once (the next impact of the item uses it). <see cref="GearValuesResult.Kept"/> says whether Kiln keeps
    /// it through saves (item data on).</returns>
    /// <remarks>What players see: the game's tooltip computes damage from the item type on each player's own game, so it
    /// shows the type's number; Kiln clients with the tooltip values on (<c>ui.tooltip.values</c>) are shown the scaled
    /// number. An arrow's or a spell's own power belongs to that item: scale the arrow or spell item for it.</remarks>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="scale"/> is outside 0.01..100 or not a number.</exception>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none or not an item entity's ref.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.gear.power</c> is off (an older Kiln, a game build
    /// without it, a fault), or the server world is not running.</exception>
    /// <example><code>
    /// var r = Items.SetPower(Items.Held(player), 1.25);   // this sword deals a quarter more
    /// if (!r.Ok) Log.Info(r.Reason!);
    /// </code></example>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearPower)]
    public static GearValuesResult SetPower(ItemInstanceRef item, double scale) =>
        SetScale(item, ItemsNative.ScalePower, scale, Caps.GearPower, "Items.SetPower");

    /// <summary>
    /// Multiplies the armour one worn item adds to its wearer (its type's armour at its level and that of its unlocked
    /// enhancements), on the server. Only this item, wherever it goes, through saves and restarts. 1 gives it its own armour
    /// back. The game's tooltip shows the type's armour (see <see cref="SetPower"/>'s remarks).
    /// </summary>
    /// <param name="item">The item (an armour piece, a ring, a shield...: whatever adds armour when worn).</param>
    /// <param name="scale"><see cref="MinScale"/> to <see cref="MaxScale"/>; 1 = its own armour.</param>
    /// <returns>Set at once (the game sums the wearer's armour again in its next pass).</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="scale"/> is outside 0.01..100 or not a number.</exception>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none or not an item entity's ref.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.gear.armor</c> is off.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearArmor)]
    public static GearValuesResult SetArmor(ItemInstanceRef item, double scale) =>
        SetScale(item, ItemsNative.ScaleArmor, scale, Caps.GearArmor, "Items.SetArmor");

    /// <summary>One item's power and armour multipliers and the values Kiln keeps for it through loads; null when the
    /// item is gone or the ref is stale. An item nobody changed reads 1, 1, null, null.</summary>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none or not an item entity's ref.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">Gear reads are off (<c>items.gear.read</c>).</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearRead)]
    public static GearValues? ValuesOf(ItemInstanceRef item)
    {
        Services.RequireTickThread("Items.ValuesOf");
        var stamp = StampOf(item, nameof(item));
        if (!Can(Caps.GearRead)) throw new KilnUnavailableException(Caps.GearRead, Capabilities.Get(Caps.GearRead).ReasonText);
        double power = 1, armor = 1;
        var t = V8Scales;
        if (t != null)
        {
            ItemsNative.GearScale s = default;
            s.Size = (uint)sizeof(ItemsNative.GearScale);
            var rc = t->GearScaleRead(item.Entity.Id.Value, stamp, &s);
            if (rc == ItemsNative.NotFound) return null;
            if (rc == ItemsNative.Ok)
            {
                power = s.Power;
                armor = s.Armor;
            }
        }
        var kept = GearKeep.Of(item);
        return new GearValues(item, power, armor, kept?.BaseLevel, kept?.DurabilityMax, kept is not null && GearKeep.Keeper is not null);
    }

    private static GearValuesResult SetScale(ItemInstanceRef item, uint which, double scale, string row, string api)
    {
        Services.RequireTickThread(api);
        if (!double.IsFinite(scale) || scale < MinScale || scale > MaxScale)
            throw new ArgumentOutOfRangeException(nameof(scale), scale, $"a multiplier is {MinScale} to {MaxScale}");
        var stamp = StampOf(item, nameof(item));
        var rc = ApplyScale(item, stamp, which, (float)scale, row, api, out var after);
        if (rc == GearValuesStatus.Done)
        {
            var kept = GearKeep.AfterScale(item, which, (float)scale);
            return new GearValuesResult(rc, item, after.Power, after.Armor, kept, "");
        }
        return new GearValuesResult(rc, item, 1, 1, false,
            rc == GearValuesStatus.Stale ? "the item is gone, or its id names another item now" : "8192 items have a multiplier already in this world");
    }

    /// <summary>The native set and its read-back (the keeper re-applies through here without keeping again).</summary>
    internal static GearValuesStatus ApplyScale(ItemInstanceRef item, uint stamp, uint which, float scale, string row, string api, out (double Power, double Armor) after)
    {
        after = (1, 1);
        var t = V8Scales;
        if (t == null)
            throw new KilnUnavailableException(row, T == null ? "kiln.items is not loaded" : "this kiln.items has no per-instance values (update Kiln)");
        if (!Can(row)) throw new KilnUnavailableException(row, Capabilities.Get(row).ReasonText);
        ItemsNative.GearScale s = default;
        s.Size = (uint)sizeof(ItemsNative.GearScale);
        s.Flags = which;
        s.Entity = item.Entity.Id.Value;
        s.Stamp = stamp;
        if ((which & ItemsNative.ScalePower) != 0) s.Power = scale;
        if ((which & ItemsNative.ScaleArmor) != 0) s.Armor = scale;
        var rc = t->GearScaleSet(&s);
        switch (rc)
        {
            case ItemsNative.Ok:
                ItemsNative.GearScale r = default;
                r.Size = (uint)sizeof(ItemsNative.GearScale);
                if (t->GearScaleRead(item.Entity.Id.Value, stamp, &r) == ItemsNative.Ok) after = (r.Power, r.Armor);
                return GearValuesStatus.Done;
            case ItemsNative.NotFound:
                return GearValuesStatus.Stale;
            case ItemsNative.BufferTooSmall:
                return GearValuesStatus.Full;
            case ItemsNative.WrongThread:
                throw new InvalidOperationException($"{api} must be called on the tick thread");
            case ItemsNative.InvalidArgument:
                throw new ArgumentException($"{api}: kiln.items refused the request's arguments");
            default:
                throw new KilnUnavailableException(row, ScaleDetail(t) is { Length: > 0 } d ? d : $"kiln.items refused it (error {rc})");
        }
    }

    // ---- the keeper's side (Kiln.Blocks' GearKeepRuntime) ----

    /// <summary>Kiln.Blocks registers who keeps the values through loads.</summary>
    internal static void RegisterKeeper(IGearKeeper keeper) => GearKeep.Keeper = keeper;

    /// <summary>Writes a kept multiplier again (the keeper's pass): no keeping, no exception for a stale item.</summary>
    internal static bool ReapplyScale(ItemInstanceRef item, uint which, double scale)
    {
        var row = which == ItemsNative.ScalePower ? Caps.GearPower : Caps.GearArmor;
        return ApplyScale(item, StampOf(item, nameof(item)), which, (float)scale, row, "kiln gear values", out _) == GearValuesStatus.Done;
    }

    /// <summary>Writes a kept level again (base outside the type's range): the keeper's pass, no keeping.</summary>
    internal static Task<GearResult> ReapplyLevel(ItemInstanceRef item, int level) =>
        WriteGear(item, GearChange.ToLevel(level), new GearWriteOptions { AnyLevel = true, Durability = DurabilityRule.Keep }, GearChangeSource.Code,
            "kiln gear values");

    /// <summary>Writes a kept max durability again: the keeper's pass, no keeping.</summary>
    internal static Task<GearResult> ReapplyDurabilityMax(ItemInstanceRef item, int max) =>
        WriteGear(item, new GearChange(null, null, null, max), new GearWriteOptions { Durability = DurabilityRule.Keep }, GearChangeSource.Code,
            "kiln gear values");

    internal static string ScaleDetail(ItemsNative.TableV8* t)
    {
        ItemsNative.ScaleStatus s = default;
        s.Size = (uint)sizeof(ItemsNative.ScaleStatus);
        return t->ScaleStatusOf(&s) == ItemsNative.Ok ? ItemsNative.Text(s.Detail, 160) : "";
    }
}

/// <summary>Which value a gear write leaves for Kiln to keep through loads (Items.SetLevel with AnyLevel,
/// Items.SetDurabilityMax).</summary>
internal enum GearKeepIntent
{
    None = 0,
    Level = 1,             // keep the base level when it is outside the type's range, else end the kept one
    DurabilityMax = 2,     // keep the written max durability
    DurabilityMaxClear = 3,// end the kept max durability (the game computes its own again)
}

/// <summary>The values Kiln keeps for one item through loads: multipliers (1 = none), a base level outside the type's
/// range, a max durability.</summary>
internal sealed record GearKept(double Power = 1, double Armor = 1, int? BaseLevel = null, int? DurabilityMax = null)
{
    public bool IsEmpty => Power == 1 && Armor == 1 && BaseLevel is null && DurabilityMax is null;
}

/// <summary>Who keeps the values (Kiln.Blocks' GearKeepRuntime registers itself; none: they last for the session).</summary>
internal interface IGearKeeper
{
    /// <summary>The kept values of an item, or null (tick thread).</summary>
    GearKept? Of(ItemInstanceRef item);

    /// <summary>Stores the item's kept values (null or empty: removes them). False when it could not (item data off).</summary>
    bool Store(ItemInstanceRef item, GearKept? kept);
}

internal static class GearKeep
{
    internal static IGearKeeper? Keeper { get; set; }

    internal static GearKept? Of(ItemInstanceRef item)
    {
        try
        {
            return Keeper?.Of(item);
        }
        catch (KilnUnavailableException)
        {
            return null;
        }
    }

    /// <summary>A gear write with a keep intent was done (tick thread, the result's Pre).</summary>
    internal static bool AfterWrite(ItemInstanceRef item, GearKeepIntent intent, GearState after)
    {
        var k = Keeper;
        if (k is null) return false;
        var have = Of(item) ?? new GearKept();
        GearKept next;
        switch (intent)
        {
            case GearKeepIntent.Level:
                var outside = after.TypeLevelMax > 0 && (after.BaseLevel < after.TypeLevelMin || after.BaseLevel > after.TypeLevelMax);
                next = have with { BaseLevel = outside ? after.BaseLevel : null };
                break;
            case GearKeepIntent.DurabilityMax:
                next = have with { DurabilityMax = after.DurabilityMax };
                break;
            case GearKeepIntent.DurabilityMaxClear:
                next = have with { DurabilityMax = null };
                break;
            default:
                return false;
        }
        if (next == have) return intent switch { GearKeepIntent.Level => next.BaseLevel is not null, GearKeepIntent.DurabilityMax => true, _ => false };
        var stored = Store(k, item, next);
        return stored && intent switch { GearKeepIntent.Level => next.BaseLevel is not null, GearKeepIntent.DurabilityMax => true, _ => false };
    }

    /// <summary>Items.SetPower / SetArmor set a multiplier (tick thread).</summary>
    internal static bool AfterScale(ItemInstanceRef item, uint which, float scale)
    {
        var k = Keeper;
        if (k is null) return false;
        var have = Of(item) ?? new GearKept();
        var next = have;
        if ((which & ItemsNative.ScalePower) != 0) next = next with { Power = scale };
        if ((which & ItemsNative.ScaleArmor) != 0) next = next with { Armor = scale };
        var ok = next == have || Store(k, item, next);
        return ok && !next.IsEmpty;
    }

    private static bool Store(IGearKeeper k, ItemInstanceRef item, GearKept next)
    {
        try
        {
            return k.Store(item, next.IsEmpty ? null : next);
        }
        catch (KilnUnavailableException)
        {
            return false;
        }
    }
}
