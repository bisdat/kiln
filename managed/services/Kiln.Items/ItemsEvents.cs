// SPDX-License-Identifier: MIT
// Kiln.Items on the event bus: ItemsGivenEvent, ItemsTakenEvent, the gear events (docs/kiln/events.md).
namespace Kiln;

/// <summary>Base of <see cref="ItemsGivenEvent"/> and <see cref="ItemsTakenEvent"/>: a Kiln.Items transaction
/// finished with something moved (Done or Partial, not a dry run). Posted on the tick thread when the transaction
/// completes (Pre of the step after it ran), before the caller's task continues. Not cancelable (it already
/// happened). Items players move themselves do not raise it.</summary>
public abstract class ItemsTransactionEvent : KilnEvent
{
    private protected ItemsTransactionEvent(ItemsResult result, string origin)
    {
        Result = result;
        Origin = origin;
    }

    public ItemsResult Result { get; }

    /// <summary>The inventory owner (player, chest, NPC).</summary>
    public EntityId Target => Result.Entity;

    public ItemId Item => Result.Item;

    /// <summary>How many were given / taken.</summary>
    public int Count => Result.Done;

    /// <summary>Id of the mod that asked ("kiln" outside mod code).</summary>
    public string Origin { get; }

    /// <summary>The item from the catalog (null when unknown).</summary>
    public ItemInfo? Info => Items.Get(Item);
}

/// <summary>Kiln.Items gave items (see <see cref="ItemsTransactionEvent"/>).</summary>
public sealed class ItemsGivenEvent : ItemsTransactionEvent
{
    internal ItemsGivenEvent(ItemsResult result, string origin) : base(result, origin) { }
    public override string ToString() => $"given {Count} x {Item} to {Target} by {Origin}";
}

/// <summary>Kiln.Items took items (see <see cref="ItemsTransactionEvent"/>).</summary>
public sealed class ItemsTakenEvent : ItemsTransactionEvent
{
    internal ItemsTakenEvent(ItemsResult result, string origin) : base(result, origin) { }
    public override string ToString() => $"taken {Count} x {Item} from {Target} by {Origin}";
}

/// <summary>Base of the gear events: an item's enhancement slots (so its rarity), its unlocked enhancements or its level
/// changed in place (<see cref="Items.SetRarity"/>, <see cref="Items.SetEnhancements"/>, <see cref="Items.SetLevel"/>, an
/// upgrade, or the game: a player's unlock at the workbench). Posted on the tick thread: for Kiln's own writes when the
/// result arrives (the Pre of the step after the write, before the caller's task continues), for the game's changes when
/// Kiln's read of the players' items sees them (one player per step, while one of these events has subscribers; items
/// outside players' inventories are not read). Only when something changed; a handler that writes the item again gets its
/// own event in a later step, never inside the dispatch (docs/kiln/modding/contract.md, "Change events"). Not cancelable
/// (it already happened). Capability <c>items.gear.read</c>.</summary>
[ServerOnly]
public abstract class GearEvent : KilnEvent
{
    private protected GearEvent(GearState before, GearState after, GearChangeSource source, string origin)
    {
        Before = before;
        After = after;
        Source = source;
        Origin = origin;
    }

    /// <summary>The item.</summary>
    public ItemInstanceRef Item => After.Item;

    /// <summary>The item before the change.</summary>
    public GearState Before { get; }

    /// <summary>The item after it.</summary>
    public GearState After { get; }

    /// <summary>What made the change.</summary>
    public GearChangeSource Source { get; }

    /// <summary>Id of the mod that wrote it ("kiln" outside mod code; "game" for <see cref="GearChangeSource.Game"/>).</summary>
    public string Origin { get; }
}

/// <summary>An item's enhancement slots, unlocked enhancements, level or (a repair) durability changed: posted for every
/// such change, before <see cref="GearRarityChangedEvent"/> and <see cref="GearEnhancedEvent"/> of the same change (see
/// <see cref="GearEvent"/>).</summary>
public sealed class GearChangedEvent : GearEvent
{
    internal GearChangedEvent(GearState before, GearState after, GearChangeSource source, string origin) : base(before, after, source, origin) { }

    /// <summary>What changed between <see cref="GearEvent.Before"/> and <see cref="GearEvent.After"/>.</summary>
    public GearChanges Changed => GearEvents.Diff(Before, After);

    /// <summary>The change in one line, for a log.</summary>
    public override string ToString() => $"gear changed ({Source} by {Origin}): {Before} -> {After}";
}

/// <summary>An item's rarity changed (its enhancement slots): see <see cref="GearEvent"/>. Also when the game's own
/// change was seen (<see cref="GearChangeSource.Game"/>).</summary>
public sealed class GearRarityChangedEvent : GearEvent
{
    internal GearRarityChangedEvent(GearState before, GearState after, GearChangeSource source, string origin) : base(before, after, source, origin) { }

    /// <summary>The rarity before.</summary>
    public ItemRarity From => Before.Rarity;

    /// <summary>The rarity now.</summary>
    public ItemRarity To => After.Rarity;

    /// <summary>The change in one line, for a log.</summary>
    public override string ToString() => $"gear rarity {From} -> {To} of {Item} ({Source} by {Origin})";
}

/// <summary>How many of an item's enhancements are unlocked changed: a player unlocked one at the workbench
/// (<see cref="GearChangeSource.Game"/>), <see cref="Items.SetEnhancements"/>, or a lower rarity took unlocked slots away
/// (see <see cref="GearEvent"/>).</summary>
public sealed class GearEnhancedEvent : GearEvent
{
    internal GearEnhancedEvent(GearState before, GearState after, GearChangeSource source, string origin) : base(before, after, source, origin) { }

    /// <summary>Unlocked before.</summary>
    public int From => Before.Unlocked;

    /// <summary>Unlocked now.</summary>
    public int To => After.Unlocked;

    /// <summary>The change in one line, for a log.</summary>
    public override string ToString() => $"gear enhancements {From} -> {To} of {Item} ({Source} by {Origin})";
}

/// <summary>Posts the gear events (one change-notice queue per kind, keyed by the item).</summary>
internal static class GearEvents
{
    internal static readonly ChangeNotices<GearChangedEvent, ItemInstanceRef> ChangedNotices = new("kiln.items gear");
    internal static readonly ChangeNotices<GearRarityChangedEvent, ItemInstanceRef> RarityNotices = new("kiln.items gear rarity");
    internal static readonly ChangeNotices<GearEnhancedEvent, ItemInstanceRef> EnhancedNotices = new("kiln.items gear enhancements");

    /// <summary>What differs between two reads of one item (pure).</summary>
    internal static GearChanges Diff(GearState before, GearState after)
    {
        var c = GearChanges.None;
        if (before.Enhancements != after.Enhancements || before.Unlocked != after.Unlocked) c |= GearChanges.Enhancements;
        if (before.Level != after.Level) c |= GearChanges.Level;
        if (before.Durability != after.Durability) c |= GearChanges.Durability;
        return c;
    }

    /// <summary>Some events of these kinds have subscribers (the gear watch reads only then).</summary>
    internal static bool Wanted =>
        Events.HasSubscribers<GearChangedEvent>() || Events.HasSubscribers<GearRarityChangedEvent>() || Events.HasSubscribers<GearEnhancedEvent>();

    /// <summary>A change took (tick thread): its events, only for what changed. Never throws.</summary>
    internal static void Changed(ItemInstanceRef item, GearState before, GearState after, GearChangeSource source, string origin)
    {
        try
        {
            if (Diff(before, after) == GearChanges.None) return;
            var subject = item.Entity.Id;
            if (Events.HasSubscribers<GearChangedEvent>()) ChangedNotices.Post(item, new GearChangedEvent(before, after, source, origin), subject);
            if (before.Rarity != after.Rarity && Events.HasSubscribers<GearRarityChangedEvent>())
                RarityNotices.Post(item, new GearRarityChangedEvent(before, after, source, origin), subject);
            if (before.Unlocked != after.Unlocked && Events.HasSubscribers<GearEnhancedEvent>())
                EnhancedNotices.Post(item, new GearEnhancedEvent(before, after, source, origin), subject);
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Error, $"kiln.items: posting a gear event failed: {e.Message}");
        }
    }
}

internal static class ItemsEvents
{
    internal static void Completed(bool take, ItemsResult result, ModContext? mod)
    {
        if (!result.Succeeded || result.DryRun || result.Done <= 0) return;
        if (take ? !Events.HasSubscribers<ItemsTakenEvent>() : !Events.HasSubscribers<ItemsGivenEvent>()) return;
        var origin = mod?.Info.Id ?? "kiln";
        KilnEvent ev = take ? new ItemsTakenEvent(result, origin) : new ItemsGivenEvent(result, origin);
        try
        {
            if (Game.IsTickThread) Events.Post(ev);
            else Events.Enqueue(ev);
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Error, $"kiln.items: posting {ev.GetType().Name} failed: {e.Message}");
        }
    }
}
