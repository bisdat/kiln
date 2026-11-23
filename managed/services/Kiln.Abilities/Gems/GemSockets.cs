// SPDX-License-Identifier: MIT
// Gem wave G4: sockets on rings, armour, shields and other gear (docs/kiln/services/gems-sockets.md). The game runs the
// gem of the item in any of the 11 equipment impact slots and saves a socket on any item; only the creation of a socket
// (weapons only), the forge's lists (weapons only) and the vanilla forge's unsocket (weapons only) stop other gear.
// Kiln widens the creation (kiln.items v7: ItemSockets), the client's forge lists (kiln.ui_values: GemForgeSockets) and
// gives an uninstall path. Nothing changes until a mod calls AllowSockets.
namespace Kiln;

/// <summary>Kinds of gear, besides weapons, that may hold a gem (<see cref="Gems.AllowSockets"/>).</summary>
[Flags]
public enum GemSocketKinds
{
    /// <summary>No gear besides weapons.</summary>
    None = 0,
    /// <summary>Rings (category Equipment, slot type Armour_Ring): gem impact slots 7 and 8.</summary>
    Rings = 1,
    /// <summary>Head, chest, hands, legs and feet armour: impact slots 2-6.</summary>
    Armour = 2,
    /// <summary>Shields: impact slot 1.</summary>
    Shields = 4,
    /// <summary>Any other gear with its own item entity (tools, backpacks, gliders, instruments...). A gem in it runs only
    /// while the item sits in an impact slot (held in the hand, for a tool).</summary>
    OtherGear = 8,
    /// <summary>Every kind above: rings, armour, shields and other gear.</summary>
    All = Rings | Armour | Shields | OtherGear,
}

/// <summary>What <see cref="Gems.AllowSockets"/> set up on this side.</summary>
/// <param name="Kinds">The kinds this mod allows now (the union over mods is <see cref="Gems.SocketKinds"/>).</param>
/// <param name="Server">The server part works here: <c>Gear.GemSlot</c>, <see cref="Gems.AddSocket"/>, the rolls and data
/// packs' <c>gemSlotOverride</c> on these kinds (kiln.items v7, feature <c>gems.sockets</c>).</param>
/// <param name="ServerReason">Why not, when <paramref name="Server"/> is false (a game client without a server world, an
/// older Kiln, an unknown game build).</param>
/// <param name="ForgeLists">The client part works here: the Gem Forge lists show gear of these kinds that has a socket.</param>
/// <param name="ForgeReason">Why not, when <paramref name="ForgeLists"/> is false (a dedicated server has no forge
/// screen).</param>
public sealed record GemSocketsSetup(GemSocketKinds Kinds, bool Server, string? ServerReason, bool ForgeLists, string? ForgeReason);

/// <summary>The outcome of <see cref="Gems.AddSocket"/>.</summary>
public enum GemSocketStatus
{
    /// <summary>The item was re-created with an empty socket: <see cref="GemSocketResult.After"/> is the new item.</summary>
    Done = 0,
    /// <summary>It already has a socket (empty or not); nothing changed.</summary>
    AlreadyHasSocket = 1,
    /// <summary>Its kind is not allowed: no mod called <see cref="Gems.AllowSockets"/> for it.</summary>
    KindNotAllowed = 2,
    /// <summary>It is worn or held in an equipment slot: unequip it first.</summary>
    Equipped = 3,
    /// <summary>The item is gone, the ref is stale, or it is in no inventory.</summary>
    NotFound = 4,
    /// <summary>It has no item entity of its own (a plain stack), or it is a gem, a dye or a customization item.</summary>
    NotGear = 5,
    /// <summary>The game refused the re-creation, or the item changed meanwhile (put on, taken in hand, given a gem
    /// slot); nothing changed (see the reason).</summary>
    Refused = 6,
    /// <summary>Sockets on other gear do not work here (see the reason); nothing changed.</summary>
    Unavailable = 7,
    /// <summary>The item was re-created (<see cref="GemSocketResult.After"/> is the new item, the old one is gone), but read
    /// back it lacks what <see cref="GemSocketResult.Missing"/> lists: its socket, or what a re-creation keeps (level,
    /// unlocked upgrades, rarity, dye, durability). kiln.items' creation hook could not apply it (for example a game build
    /// whose item layout Kiln does not know); the item is as the game made it.</summary>
    Incomplete = 8,
}

/// <summary>The outcome of <see cref="Gems.AddSocket"/>.</summary>
/// <param name="Status">What happened.</param>
/// <param name="Before">The item asked for.</param>
/// <param name="After">With <see cref="GemSocketStatus.Done"/> or <see cref="GemSocketStatus.Incomplete"/>: the new item (a
/// new item entity in the same slot; the old one is gone). Otherwise <paramref name="Before"/>.</param>
/// <param name="Reason">Why, in words, for anything but Done; "" for Done.</param>
public sealed record GemSocketResult(GemSocketStatus Status, ItemInstanceRef Before, ItemInstanceRef After, string Reason)
{
    /// <summary>True when the new item has its socket and everything a re-creation keeps (read back).</summary>
    public bool Succeeded => Status == GemSocketStatus.Done;

    /// <summary>With <see cref="GemSocketStatus.Incomplete"/>: what the new item lacks, one entry each (<c>"a socket"</c>,
    /// <c>"level 34 (it has 30)"</c>, <c>"4 unlocked upgrades (it has 0)"</c>, <c>"rarity Epic (it has Common)"</c>,
    /// <c>"dye ..."</c>, <c>"durability 50% (it has 100%)"</c>, or that it could not be read back). Empty otherwise.</summary>
    public IReadOnlyList<string> Missing { get; init; } = [];

    /// <summary>For a log line.</summary>
    public override string ToString() => Status switch
    {
        GemSocketStatus.Done => $"Done: {Before} -> {After}",
        GemSocketStatus.Incomplete => $"Incomplete: {Before} -> {After}, missing {string.Join(", ", Missing)}",
        _ => $"{Status}: {Reason}",
    };
}

/// <summary>One gem <see cref="Gems.ReturnSocketedGems"/> took out of a non-weapon socket (or could not).</summary>
/// <param name="Carrier">The ring, armour piece, shield or other gear that held it.</param>
/// <param name="Gem">The gem item.</param>
/// <param name="Level">The gem's level.</param>
/// <param name="Returned">True when the gem is in the owner's inventory now.</param>
/// <param name="Reason">Why not, when <paramref name="Returned"/> is false.</param>
public sealed record GemReturned(ItemInstanceRef Carrier, ItemId Gem, int Level, bool Returned, string? Reason);

public static partial class Gems
{
    /// <summary>The kinds any mod allows sockets on now (this side).</summary>
    [AnyThread]
    public static GemSocketKinds SocketKinds => (GemSocketKinds)ItemSockets.AllowedKinds;

    /// <summary>
    /// Lets gear of <paramref name="kinds"/> hold a gem, for as long as the calling mod is loaded (a later call replaces
    /// this mod's; <see cref="GemSocketKinds.None"/> ends it). Call it on both sides (a mod with side Both): on the server it
    /// lets <c>Gear.GemSlot</c> (<see cref="Items.Give(EntityId, ItemId, int, Gear?, GiveOptions, SlotId?)"/>) and
    /// <see cref="AddSocket"/> give these kinds a socket and, with <paramref name="rollChance"/> above 0, makes every new
    /// item of these kinds the game creates (loot, crafting) roll an empty socket with that chance (weapons keep the
    /// game's own roll; the highest chance any mod asks for wins). On a game client it widens the Gem Forge's WEAPONS list
    /// and its socketed rows to gear of these kinds that has a socket, so players can socket, unsocket, upgrade and recycle
    /// there. Items whose game data sets <c>gemSlotOverride</c> get a socket whatever this says (see the guide).
    /// Balance is the mod's: a gem reacts to every hit of its wearer, not only the carrier's, and up to 11 gems can run at
    /// once (held item, shield, 5 armour pieces, 2 rings, arrows, spell).
    /// Call it in OnLoad or on the tick thread: the allowance belongs to the mod running there and ends with it. From
    /// another thread no mod is known, so it would be Kiln's own and outlive the mod.
    /// </summary>
    /// <param name="kinds">The kinds; <see cref="GemSocketKinds.None"/> ends this mod's allowance and rolls.</param>
    /// <param name="rollChance">0..1: the chance a new item of these kinds rolls an empty socket when the game creates it
    /// (server). 0 (the default): only Kiln's own calls give sockets.</param>
    /// <exception cref="ArgumentOutOfRangeException">An unknown kind bit, or a chance outside 0..1.</exception>
    [TickThreadOnly]
    [RequiresCapability(SocketsCapability)]
    public static GemSocketsSetup AllowSockets(GemSocketKinds kinds, double rollChance = 0)
    {
        if ((kinds & ~GemSocketKinds.All) != 0) throw new ArgumentOutOfRangeException(nameof(kinds), kinds, "unknown kind");
        if (!(rollChance >= 0 && rollChance <= 1)) throw new ArgumentOutOfRangeException(nameof(rollChance), rollChance, "a chance is 0..1");
        var mod = ModContext.CurrentMod;
        var owner = Services.CallerOwner;
        ItemSockets.SetAllowed(mod, (uint)kinds);
        Track(mod, owner);
        // the server part: the kinds above, the rolls in kiln.items
        var serverWhy = ItemSockets.WhyOff;
        if (serverWhy == null)
        {
            var c = kinds == GemSocketKinds.None ? 0f : (float)rollChance;
            var rc = ItemSockets.SetRoll(owner, Has(kinds, GemSocketKinds.Rings) ? c : 0, Has(kinds, GemSocketKinds.Armour) ? c : 0,
                Has(kinds, GemSocketKinds.Shields) ? c : 0, Has(kinds, GemSocketKinds.OtherGear) ? c : 0);
            if (rc != 0) serverWhy = $"kiln.items refused the socket rolls ({rc})";
        }
        // the client part: the forge's lists (this mod's kinds; kiln.ui.values ORs every mod's and drops a mod's at unload)
        var (forge, forgeWhy) = GemForgeSockets.Widen(ForgeCategoryMask(kinds));
        return new GemSocketsSetup(kinds, serverWhy == null, serverWhy, forge, forge ? null : forgeWhy);
    }

    /// <summary>The capability row of sockets on other gear: on a server the createPide hook's
    /// (sigdb/symbols/server.d/gems-sockets.json), on a client the forge lists' (client.d/gems-sockets.json).</summary>
    public const string SocketsCapability = "gems.sockets";

    /// <summary>
    /// Gives an existing piece of gear without a socket an empty one: the game cannot add a component to an item
    /// entity, so Kiln re-creates the item in the same slot, in one inventory transaction (the old item entity is taken,
    /// the new one created; both or neither). Kept: the item, its level, rarity, unlocked upgrades, durability (as a
    /// fraction of its max) and dye. Not kept: the item entity itself (anything keyed to the old entity is not moved:
    /// Kiln.Saves' instance data on it counts as destroyed, so a mod that keeps data on gear reads it first and sets it on
    /// <see cref="GemSocketResult.After"/>), water in a can. The item must sit in an inventory (a backpack, an action bar,
    /// a chest), not worn nor held in the hand; kiln.items checks that again on the step it runs (put on or taken in hand
    /// meanwhile: <see cref="GemSocketStatus.Refused"/>, nothing changed). Weapons are accepted always; other gear needs its
    /// kind allowed (<see cref="AllowSockets"/>). Kiln reads the new item back: <see cref="GemSocketStatus.Done"/> only when
    /// it has its socket and kept everything above that the game lets Kiln read; otherwise
    /// <see cref="GemSocketStatus.Incomplete"/> with <see cref="GemSocketResult.Missing"/> (the item is re-created either
    /// way). It reads once the world has run a step with the new item (the game finishes a new item in that step).
    /// Completes on the tick thread two to four steps later.
    /// </summary>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(SocketsCapability)]
    public static Task<GemSocketResult> AddSocket(ItemInstanceRef item)
    {
        if (item.IsNone) throw new ArgumentException("no item", nameof(item));
        var why = ItemSockets.WhyOff;
        if (why != null) return Done(GemSocketStatus.Unavailable, item, why);
        ItemSockets.GearState g;
        try { g = ItemSockets.Read(item); }
        catch (KilnUnavailableException e) { return Done(GemSocketStatus.Unavailable, item, e.Message); }
        var info = Items.Get(item.Item);
        var decision = DecideAddSocket(g, info?.Category, info?.EquipSlot ?? 0, ItemSockets.AllowedKinds);
        if (decision is { } refused) return Done(refused.Status, item, refused.Reason);
        var rarity = ItemSockets.RarityAt(g.Holder, g.Slot, item.Entity.Id);
        var rx = ItemSockets.RecreateRequest(g.Holder, g, rarity, socket: true);
        var done = new TaskCompletionSource<GemSocketResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        ItemSockets.Recreate(g.Holder, item.Entity.Id, rx).ContinueWith(t =>
            {
                var r = t.IsCompletedSuccessfully
                    ? ResultOf(item, t.Result)
                    : new GemSocketResult(GemSocketStatus.Unavailable, item, item, t.Exception?.GetBaseException().Message ?? "canceled");
                if (r.Status != GemSocketStatus.Done) done.TrySetResult(r);
                else AfterWorldStep(() => done.TrySetResult(ReadBack(r, g, rarity)));  // the read needs the tick thread
            },
            CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        return done.Task;
    }

    // The re-created item as kiln.items reads it now, checked against what it was (tick thread).
    private static GemSocketResult ReadBack(GemSocketResult done, ItemSockets.GearState before, ItemRarity? rarityBefore)
    {
        try
        {
            var after = ItemSockets.Read(done.After);
            var rarity = after.Found ? ItemSockets.RarityAt(after.Holder, after.Slot, done.After.Entity.Id) : null;
            return CheckRecreated(done, before, rarityBefore, after, rarity, null);
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException)
        {
            return CheckRecreated(done, before, rarityBefore, null, null, e.Message);
        }
    }

    /// <summary>Runs <paramref name="run"/> once on the tick thread, at a service tick after the world has run a whole
    /// step since this call. The game finishes a new item entity (its ItemState rarity and level, the Level attribute's
    /// values) in the world step after the transaction that made it: at the result's delivery, in a step's Pre, it still
    /// reads Common, level 0 (lab gems-int probe 2026-11-16; on a client hosting its world the read-back ran 4 ms after
    /// the delivery and saw "level 40, Common"). Any thread: called in the delivery step's Pre, the last service tick is
    /// that step's (it ran before the delivery) or the one before (it has not run yet), so the handler waits for the second
    /// step after the last one. A world that starts again (its steps count from 1 again) runs it at once.</summary>
    internal static void AfterWorldStep(Action run)
    {
        var last = ServiceTick.Step;
        var due = last + 2;
        var ran = 0;
        IDisposable? tick = null;
        tick = ServiceTick.Add("kiln.abilities gem socket check", (in TickInfo t) =>
        {
            if (t.Step < due && t.Step >= last) return;
            if (Interlocked.Exchange(ref ran, 1) != 0) return;
            tick?.Dispose();
            run();
        });
        if (Volatile.Read(ref ran) != 0) tick.Dispose();  // it ran before the handle was set
    }

    /// <summary>A re-creation kiln.items reported Done, checked against the new item read back (<paramref name="after"/>;
    /// null with <paramref name="readError"/> when it could not be read): Done when it has a socket and kept the level,
    /// unlocked upgrades, rarity, dye and durability (each compared only where both reads have it), else Incomplete with
    /// what is missing. Anything but Done is passed through. Pure.</summary>
    internal static GemSocketResult CheckRecreated(GemSocketResult done, ItemSockets.GearState before, ItemRarity? rarityBefore,
        ItemSockets.GearState? after, ItemRarity? rarityAfter, string? readError)
    {
        if (done.Status != GemSocketStatus.Done) return done;
        var missing = new List<string>();
        if (after is not { Found: true } a)
            missing.Add($"the new item could not be read back ({readError ?? "it is gone, or its ref names another item"})");
        else
        {
            if (!a.HasSocket) missing.Add("a socket");
            if (before.Level > 0 && a.Level != before.Level) missing.Add($"level {before.Level} (it has {a.Level})");
            if (a.PerksUnlocked < before.PerksUnlocked)
                missing.Add($"{before.PerksUnlocked} unlocked upgrade{(before.PerksUnlocked == 1 ? "" : "s")} (it has {a.PerksUnlocked})");
            if (rarityBefore is { } rb && rarityAfter is { } ra && ra != rb) missing.Add($"rarity {rb} (it has {ra})");
            if (!before.Dye.IsNone && a.Dye != before.Dye) missing.Add($"dye {before.Dye} (it has {(a.Dye.IsNone ? "none" : a.Dye)})");
            if (before is { HasDurability: true, DurabilityMax: > 0 } && a is { HasDurability: true, DurabilityMax: > 0 })
            {
                var was = (double)before.Durability / before.DurabilityMax;
                var now = (double)a.Durability / a.DurabilityMax;
                if (Math.Abs(was - now) > 0.01 + 1.0 / a.DurabilityMax)
                    missing.Add($"durability {Math.Round(was * 100)}% (it has {Math.Round(now * 100)}%)");
            }
        }
        return missing.Count == 0
            ? done
            : new GemSocketResult(GemSocketStatus.Incomplete, done.Before, done.After, $"the new item lacks: {string.Join(", ", missing)}")
            {
                Missing = missing,
            };
    }

    /// <summary>
    /// The uninstall path: takes every gem out of the non-weapon gear of <paramref name="owner"/>'s inventories (worn
    /// included) and puts it into the owner's inventory, free of charge. The vanilla forge cannot unsocket a ring or an
    /// armour piece, so a server that stops using a socket mod (or a player who wants the gems back) runs this first:
    /// sockets on other gear stay in the save and keep working without Kiln, but without Kiln nobody can get those gems
    /// out. It runs the game's own Remove move without a forge (the forge actions' service, kiln.gemcraft's forgeless
    /// return, capability <c>gems.craft</c>; each return is also a <see cref="GemCraftedEvent"/> with no forge); one entry
    /// per gem, in inventory order, <see cref="GemReturned.Returned"/> false with the reason for a gem that did not come back
    /// (no free backpack slot, <c>gems.craft</c> off). The owner must be a player character. Weapons are left alone. Await
    /// it from mod code (the task itself completes on a worker thread).
    /// </summary>
    [ServerOnly]
    [TickThreadOnly]
    public static Task<IReadOnlyList<GemReturned>> ReturnSocketedGems(EntityId owner)
    {
        if (owner.IsNone) throw new ArgumentException("no owner", nameof(owner));
        var found = new List<(ItemInstanceRef Carrier, ItemId Gem, int Level)>();
        foreach (var info in ItemInstances.ReadHolder(owner, out _))
        {
            if (info.Record.Gem.IsNone || info.Item.IsNone) continue;
            if (Items.Get(info.Item.Item)?.Category is ItemCategory.Weapons) continue;  // the forge handles weapons
            found.Add((info.Item, info.Record.Gem, info.Record.GemLevel));
        }
        if (found.Count == 0) return Task.FromResult<IReadOnlyList<GemReturned>>([]);
        var tasks = new List<Task<GemReturned>>(found.Count);
        foreach (var (carrier, gem, level) in found)
        {
            Task<(bool Ok, string? Reason)>? unsocket = null;
            UnsocketForReturn(owner, carrier, ref unsocket);
            tasks.Add(unsocket == null
                ? Task.FromResult(new GemReturned(carrier, gem, level, false,
                    "this Kiln has no gem forge actions (Gems.Unsocket, capability gems.craft)"))
                : unsocket.ContinueWith(t => t.IsCompletedSuccessfully
                        ? new GemReturned(carrier, gem, level, t.Result.Ok, t.Result.Ok ? null : t.Result.Reason)
                        : new GemReturned(carrier, gem, level, false, t.Exception?.GetBaseException().Message ?? "canceled"),
                    CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default));
        }
        return Task.WhenAll(tasks).ContinueWith(t => (IReadOnlyList<GemReturned>)t.Result,
            CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }

    /// <summary>The forge actions' free unsocket of one carrier for <see cref="ReturnSocketedGems"/>: the gem goes to
    /// <paramref name="owner"/>'s inventory, no essence is paid. Implemented next to <c>Gems.Unsocket</c> (GemCrafting.cs);
    /// left null when that part is not in this build.</summary>
    static partial void UnsocketForReturn(EntityId owner, ItemInstanceRef carrier, ref Task<(bool Ok, string? Reason)>? unsocket);

    // ---- plumbing ----

    private static bool Has(GemSocketKinds kinds, GemSocketKinds k) => (kinds & k) != 0;

    private static readonly HashSet<ModContext> Tracked = new();

    // A mod's rolls end with it (its allowance ends in ItemSockets, its forge mask in kiln.ui.values).
    private static void Track(ModContext? mod, ulong owner)
    {
        if (mod == null) return;
        lock (Tracked)
        {
            if (!Tracked.Add(mod)) return;
        }
        mod.Retiring += () =>
        {
            lock (Tracked) Tracked.Remove(mod);
            if (ItemSockets.HasV7) ItemSockets.SetRoll(owner, 0, 0, 0, 0);
        };
    }

    /// <summary>The ItemCategory bits of the forge lists for <paramref name="kinds"/> (bit = 1 &lt;&lt; category, Weapons is
    /// the game's own): Equipment for rings, armour and shields; Equipment, Tools, Instrument, FishingRod, BuildTools and
    /// Ammunition for other gear. Only items with a socket show, so a wide mask shows nothing new by itself.</summary>
    internal static uint ForgeCategoryMask(GemSocketKinds kinds)
    {
        uint m = 0;
        if ((kinds & (GemSocketKinds.Rings | GemSocketKinds.Armour | GemSocketKinds.Shields)) != 0) m |= 1u << (int)ItemCategory.Equipment;
        if ((kinds & GemSocketKinds.OtherGear) != 0)
            foreach (var c in (ItemCategory[])[ItemCategory.Equipment, ItemCategory.Tools, ItemCategory.Instrument,
                         ItemCategory.FishingRod, ItemCategory.BuildTools, ItemCategory.Ammunition])
                m |= 1u << (int)c;
        return m;
    }

    /// <summary>Why AddSocket would refuse this item before asking kiln.items, or null to go on. Pure.</summary>
    internal static (GemSocketStatus Status, string Reason)? DecideAddSocket(ItemSockets.GearState g, ItemCategory? category,
        byte equipSlot, uint allowedKinds)
    {
        if (!g.Found) return (GemSocketStatus.NotFound, "the item is gone, or the ref names another item now");
        if (!g.Placed || g.Holder.IsNone) return (GemSocketStatus.NotFound, "the item is in no inventory");
        if (category is not { } cat) return (GemSocketStatus.NotFound, $"item {g.Item} is not in the item catalog");
        if (g.Equipped) return (GemSocketStatus.Equipped, "the item is worn or held in an equipment slot: unequip it first");
        if (g.HasSocket) return (GemSocketStatus.AlreadyHasSocket, g.Gem.IsNone ? "it already has an empty socket" : $"it already holds gem {g.Gem}");
        if (cat == ItemCategory.Weapons) return null;
        var kind = ItemSockets.KindOf(cat, equipSlot);
        if (kind == 0) return (GemSocketStatus.NotGear, $"a {cat} item never holds a gem");
        if ((allowedKinds & kind) == 0)
            return (GemSocketStatus.KindNotAllowed, $"{(GemSocketKinds)kind} may not hold gems: call Gems.AllowSockets(GemSocketKinds.{(GemSocketKinds)kind}) first");
        return null;
    }

    internal static GemSocketResult ResultOf(ItemInstanceRef before, ItemsResult r)
    {
        if (r.Status == ItemsStatus.Done && r.Changes.Count > 0)
            return new GemSocketResult(GemSocketStatus.Done, before, r.Changes[0].Ref, "");
        var status = r.Status switch
        {
            ItemsStatus.Unavailable or ItemsStatus.Dropped => GemSocketStatus.Unavailable,
            ItemsStatus.SlotChanged or ItemsStatus.NoInventory => GemSocketStatus.NotFound,
            ItemsStatus.Invalid => GemSocketStatus.Refused,
            _ => GemSocketStatus.Refused,
        };
        return new GemSocketResult(status, before, before, r.Message.Length > 0 ? r.Message : r.Status.ToString());
    }

    private static Task<GemSocketResult> Done(GemSocketStatus s, ItemInstanceRef item, string why) =>
        Task.FromResult(new GemSocketResult(s, item, item, why));
}
