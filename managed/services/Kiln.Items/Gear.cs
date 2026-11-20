// SPDX-License-Identifier: MIT
// An item's rarity, enhancements and level, read and written in place: kiln.items v8 (native/core/abi/services/kiln_items.h
// "gear"). The native side writes inside its serial program kiln_items_gear through the game's tracked writes, so players
// and the save get the change like the game's own; the game derives the shown rarity, level and damage in the next step
// (docs/re/items.md section 19). Capabilities items.gear.read, items.gear.write, items.gear.durability.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

public static unsafe partial class Items
{
    /// <summary>The highest number of enhancement slots (and so rarity + 1): the game's five perk slots.</summary>
    internal const int MaxEnhancements = 5;

    /// <summary>The highest level a gear write sets: the save record keeps the level's low byte.</summary>
    internal const int MaxGearLevel = 255;

    /// <summary>The game's <c>ItemStateFlag.HasWeaponGemSlot</c> as a mask of <c>ItemState.itemState</c> (reflection, both
    /// builds: the equipment dossier, section 2.2).</summary>
    internal const ushort ItemStateHasGemSlot = 0x100;

    private static ItemsNative.TableV8* _v8;

    // The v8 table, or null when the native service is older.
    internal static ItemsNative.TableV8* V8
    {
        get
        {
            if (_v8 != null) return _v8;
            var t = T;
            if (t == null || t->Header.Size < ItemsNative.V8GearEnd) return null;  // the gear functions; the rolls after them (336) are P5's
            return _v8 = (ItemsNative.TableV8*)t;
        }
    }

    /// <summary>True when <see cref="GearOf"/> can read (capability <c>items.gear.read</c>).</summary>
    public static bool GearReadAvailable => Can(Caps.GearRead);

    /// <summary>True when <see cref="SetRarity"/>, <see cref="SetEnhancements"/> and <see cref="SetLevel"/> can write
    /// (capability <c>items.gear.write</c>).</summary>
    public static bool GearWriteAvailable => Can(Caps.GearWrite);

    /// <summary>
    /// One item's rarity, enhancements, level and durability now, as the server's world has them; null when the item is
    /// gone or the ref is stale (its id names another item now).
    /// </summary>
    /// <param name="item">The item: <see cref="Held"/>, <see cref="Equipped"/>, a slot's <see cref="InventorySlot.Ref"/>,
    /// a give result's <see cref="ItemChange.Ref"/>.</param>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none, or not a ref of an item entity.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.gear.read</c> is off (an older Kiln, a game build
    /// without gear writes, item instances off), or the server world is not running.</exception>
    /// <example><code>
    /// var g = Items.GearOf(Items.Held(player));
    /// if (g is { Rarity: not ItemRarity.Legendary }) Log.Info($"{g.Rarity}, {g.Unlocked}/{g.Enhancements} unlocked");
    /// </code></example>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearRead)]
    public static GearState? GearOf(ItemInstanceRef item)
    {
        Services.RequireTickThread("Items.GearOf");
        var stamp = StampOf(item, nameof(item));
        var t = RequireGearRead("Items.GearOf");
        return ReadGear(t, item, stamp, "Items.GearOf");
    }

    /// <summary>The item's rarity now (the game's itemRarityOf: its enhancement slots - 1, or its type's rarity when it has
    /// none); null when the item is gone or the ref is stale. The same as <see cref="GearOf"/>'s
    /// <see cref="GearState.Rarity"/>.</summary>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none, or not a ref of an item entity.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.gear.read</c> is off.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearRead)]
    public static ItemRarity? RarityOf(ItemInstanceRef item) => GearOf(item)?.Rarity;

    /// <summary>
    /// Sets an item's rarity in place: the same item entity, its item data and affixes stay on it. The game keeps the
    /// rarity as the number of enhancement slots, so a higher rarity adds slots (locked: the player unlocks each at the
    /// workbench as usual, paying its cost, and gets a level and the perk for it) and a lower one takes slots away, the
    /// unlocked ones among them (the item's level comes down by as many, as the game's own downgrade does).
    /// </summary>
    /// <param name="item">The item.</param>
    /// <param name="rarity">The rarity.</param>
    /// <param name="options">Durability (default: the game computes max durability again at the new rarity, the share
    /// kept), empty slots (default: refused, <see cref="GearStatus.NoSlot"/>), equipped items (default: allowed), dry run.</param>
    /// <returns>Completes on the tick thread one step after the step the write ran in. A refusal is a result
    /// (<see cref="GearResult.Ok"/> false): the item is then as it was. The game shows the new rarity from its next step;
    /// <see cref="GearRarityChangedEvent"/> is posted when it changed.</returns>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none or not an item entity's ref, or
    /// <paramref name="rarity"/> is no <see cref="ItemRarity"/>.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or the calling mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.gear.write</c> is off (or
    /// <c>items.gear.durability</c> for a durability rule other than <see cref="DurabilityRule.Keep"/>).</exception>
    /// <example><code>
    /// GearResult r = await Items.SetRarity(Items.Held(player), ItemRarity.Epic);
    /// if (!r.Ok) Log.Info($"not changed: {r.Reason}");
    /// </code></example>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearWrite)]
    public static Task<GearResult> SetRarity(ItemInstanceRef item, ItemRarity rarity, GearWriteOptions? options = null)
    {
        if (!Enum.IsDefined(rarity)) throw new ArgumentOutOfRangeException(nameof(rarity), rarity, "not an item rarity");
        return WriteGear(item, GearChange.ToRarity(rarity), options, GearChangeSource.Code, "Items.SetRarity");
    }

    /// <summary>
    /// Sets how many of an item's enhancement slots are unlocked, as the workbench does without its cost: each unlocked
    /// slot is one level, so the level moves by as many as the count does, and the perks of the unlocked slots act.
    /// </summary>
    /// <param name="item">The item.</param>
    /// <param name="unlocked">0 to 5; more than the item has slots ends <see cref="GearStatus.OutOfRange"/>.</param>
    /// <param name="options">As <see cref="SetRarity"/>'s.</param>
    /// <returns>As <see cref="SetRarity"/>; <see cref="GearEnhancedEvent"/> is posted when the count changed.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="unlocked"/> is outside 0..5.</exception>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none or not an item entity's ref.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or the calling mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">As <see cref="SetRarity"/>.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearWrite)]
    public static Task<GearResult> SetEnhancements(ItemInstanceRef item, int unlocked, GearWriteOptions? options = null)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(unlocked);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(unlocked, MaxEnhancements);
        return WriteGear(item, GearChange.ToUnlocked(unlocked), options, GearChangeSource.Code, "Items.SetEnhancements");
    }

    /// <summary>
    /// Sets an item's level, the unlocked enhancements included (the level the game shows). The level without them (the
    /// base) must be inside the item type's level range, unless <see cref="GearWriteOptions.AnyLevel"/> is set: the game
    /// sets a level outside the range back when the world loads, so Kiln keeps such a level with the item (item data) and
    /// writes it again after every load. Without the option such a write ends <see cref="GearStatus.OutOfRange"/>.
    /// </summary>
    /// <param name="item">The item.</param>
    /// <param name="level">1 to 255.</param>
    /// <param name="options">As <see cref="SetRarity"/>'s (empty slots do not apply).</param>
    /// <returns>As <see cref="SetRarity"/>; <see cref="GearChangedEvent"/> is posted when the level changed.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="level"/> is outside 1..255.</exception>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none or not an item entity's ref.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or the calling mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">As <see cref="SetRarity"/>.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearWrite)]
    public static Task<GearResult> SetLevel(ItemInstanceRef item, int level, GearWriteOptions? options = null)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(level, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(level, MaxGearLevel);
        // a base level a load would not give back is kept by Kiln (GearKeep) when the write is done; a level inside the
        // range ends a kept one (the game keeps it itself)
        return WriteGear(item, GearChange.ToLevel(level), options, GearChangeSource.Code, "Items.SetLevel", GearKeepIntent.Level);
    }

    /// <summary>
    /// Sets one item's max durability in place, keeping its current durability at the same share (a half-worn item stays
    /// half worn; <see cref="DurabilityRule.Repair"/>: full at the new max). The game computes max durability again when the
    /// world loads (and after a rarity change with <see cref="DurabilityRule.KeepRatio"/>), so Kiln keeps the value with
    /// the item (item data) and writes it again after every load and such a change. <c>null</c> gives the item back its
    /// game-computed max (the game computes it in its next step, the share kept) and ends the kept value.
    /// </summary>
    /// <param name="item">The item.</param>
    /// <param name="max">1 to 1,000,000, or null for the game's own value.</param>
    /// <param name="options"><see cref="GearWriteOptions.Durability"/>: <see cref="DurabilityRule.Repair"/> fills it;
    /// any other rule keeps the share. Equipped items and dry runs as <see cref="SetRarity"/>.</param>
    /// <returns>Completes on the tick thread one step after the write. A refusal is a result: <see cref="GearStatus.NotGear"/>
    /// (no durability), <see cref="GearStatus.Refused"/> (the game has not computed this item's durability yet: a new item,
    /// in its first step).</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="max"/> is outside 1..1,000,000.</exception>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none or not an item entity's ref.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or the calling mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.gear.write</c> is off (and
    /// <c>items.gear.durability</c> for <c>null</c>).</exception>
    /// <example><code>
    /// await Items.SetDurabilityMax(Items.Held(player), 900);                 // this sword lasts longer, wherever it goes
    /// </code></example>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.GearWrite)]
    public static Task<GearResult> SetDurabilityMax(ItemInstanceRef item, int? max, GearWriteOptions? options = null)
    {
        if (max is { } m)
        {
            ArgumentOutOfRangeException.ThrowIfLessThan(m, 1, nameof(max));
            ArgumentOutOfRangeException.ThrowIfGreaterThan(m, ItemsNative.GearMaxDurability, nameof(max));
            return WriteGear(item, new GearChange(null, null, null, m), options, GearChangeSource.Code, "Items.SetDurabilityMax",
                GearKeepIntent.DurabilityMax);
        }
        // back to the game's own max: a refresh (the game computes it again, the share kept), the kept value ended
        var o = options ?? GearWriteOptions.Default;
        if (o.Durability == DurabilityRule.Keep) o = o with { Durability = DurabilityRule.KeepRatio };
        return WriteGear(item, default, o, GearChangeSource.Code, "Items.SetDurabilityMax", GearKeepIntent.DurabilityMaxClear);
    }

    // ------------------------------------------------------------------------------------------- plumbing ------

    /// <summary>What one gear write asks for: any of the slots (so the rarity), the unlocked count and the level, in one
    /// native request (the upgrade action asks for slots and unlocked together).</summary>
    internal readonly record struct GearChange(int? Enhancements, int? Unlocked, int? Level, int? DurabilityMax = null)
    {
        public static GearChange ToRarity(ItemRarity rarity) => new((int)rarity + 1, null, null);
        public static GearChange ToUnlocked(int unlocked) => new(null, unlocked, null);
        public static GearChange ToLevel(int level) => new(null, null, level);
    }

    /// <summary>The native request of a change (pure: the unit tests build it without a table).</summary>
    /// <exception cref="ArgumentOutOfRangeException">A value outside its range (slots 1..5, unlocked 0..5, level 1..255), or
    /// an unknown durability rule.</exception>
    internal static ItemsNative.GearRequest GearRequestOf(ItemInstanceRef item, uint stamp, GearChange change, GearWriteOptions options)
    {
        var r = new ItemsNative.GearRequest { Size = (uint)sizeof(ItemsNative.GearRequest), Entity = item.Entity.Id.Value, Stamp = stamp, Item = item.Item.Value };
        if (change.Enhancements is { } a)
        {
            if (a is < 1 or > MaxEnhancements) throw new ArgumentOutOfRangeException(nameof(change), a, "an item has 1 to 5 enhancement slots");
            r.Flags |= ItemsNative.GearAvailable;
            r.Available = (byte)a;
        }
        if (change.Unlocked is { } u)
        {
            if (u is < 0 or > MaxEnhancements) throw new ArgumentOutOfRangeException(nameof(change), u, "0 to 5 enhancements can be unlocked");
            r.Flags |= ItemsNative.GearUnlocked;
            r.Unlocked = (byte)u;
        }
        if (change.Level is { } l)
        {
            if (l is < 1 or > MaxGearLevel) throw new ArgumentOutOfRangeException(nameof(change), l, "a level is 1 to 255");
            r.Flags |= ItemsNative.GearLevel;
            r.Level = (uint)l;
        }
        if (options.AnyLevel)
        {
            if (change.Level is null) throw new ArgumentException("GearWriteOptions.AnyLevel applies to Items.SetLevel only", nameof(options));
            r.Flags |= ItemsNative.GearAnyLevel;
        }
        if (change.DurabilityMax is { } dm)
        {
            if (dm is < 1 or > ItemsNative.GearMaxDurability) throw new ArgumentOutOfRangeException(nameof(change), dm, "a max durability is 1 to 1,000,000");
            r.Flags |= ItemsNative.GearDurabilityMax;
            r.DurabilityMax = dm;
            // the game's recompute (KeepRatio's refresh) would replace the max: the share is kept by the write itself
            if (!Enum.IsDefined(options.Durability)) throw new ArgumentOutOfRangeException(nameof(options), options.Durability, "not a durability rule");
            if (options.Durability == DurabilityRule.Repair) r.Flags |= ItemsNative.GearDurabilityRepair;
        }
        else
        {
            r.Flags |= options.Durability switch
            {
                DurabilityRule.KeepRatio => ItemsNative.GearDurabilityRefresh,
                DurabilityRule.Keep => 0u,
                DurabilityRule.Repair => ItemsNative.GearDurabilityRepair,
                _ => throw new ArgumentOutOfRangeException(nameof(options), options.Durability, "not a durability rule"),
            };
        }
        if (options.AllowEmptySlot) r.Flags |= ItemsNative.GearForceSlot;
        if (options.RefuseEquipped) r.Flags |= ItemsNative.GearNotEquipped;
        if (options.DryRun) r.Flags |= ItemsNative.GearDryRun;
        return r;
    }

    /// <summary>The kiln.items stamp an item ref carries (its serial in the Items place).</summary>
    /// <exception cref="ArgumentException">None, or not a ref of an item entity.</exception>
    internal static uint StampOf(ItemInstanceRef item, string param)
    {
        if (item.IsNone) throw new ArgumentException("no item", param);
        if (IdentitySources.PlaceOf(item.Entity.Serial) != EntitySourceOrder.Items || (item.Entity.Serial & IdentitySources.MaxStampValue) > uint.MaxValue)
            throw new ArgumentException($"{item} is not a ref of an item entity (its stamp is not kiln.items'): take it from Items.Held, a slot's Ref or a give result", param);
        return (uint)(item.Entity.Serial & IdentitySources.MaxStampValue);
    }

    private static GearState? ReadGear(ItemsNative.TableV8* t, ItemInstanceRef item, uint stamp, string api)
    {
        ItemsNative.GearState s = default;
        var rc = t->GearRead(item.Entity.Id.Value, stamp, &s);
        switch (rc)
        {
            case ItemsNative.Ok:
                return item.Item.IsNone || s.Item == item.Item.Value ? StateOf(item, in s) : null;
            case ItemsNative.NotFound:
                return null;
            case ItemsNative.Unavailable:
                throw new KilnUnavailableException(Caps.GearRead, $"{api}: kiln.items cannot read gear right now (the server world is not running, or item instances are off)");
            case ItemsNative.WrongThread:
                throw new InvalidOperationException($"{api} must be called on the tick thread");
            default:
                throw new InvalidOperationException($"{api} failed ({rc})");
        }
    }

    /// <summary>The managed state of a native one (pure: the unit tests build native states by hand). The ref is the one
    /// asked about, with the type the read found when it named none.</summary>
    internal static GearState StateOf(ItemInstanceRef item, in ItemsNative.GearState s)
    {
        var perks = (s.Flags & ItemsNative.GearHasPerks) != 0;
        var hasLevel = (s.Flags & ItemsNative.GearHasLevel) != 0;
        var durability = (s.Flags & ItemsNative.GearHasDurability) != 0;
        int available = perks ? Math.Min((int)s.Available, MaxEnhancements) : 0, unlocked = perks ? Math.Min((int)s.Unlocked, available) : 0;
        var level = hasLevel ? (int)Math.Min(s.Level, int.MaxValue) : s.LevelUi;
        var type = Rarity(s.TypeRarity) ?? ItemRarity.Common;
        var slots = new PerkSlot[MaxEnhancements];
        for (var i = 0; i < MaxEnhancements; i++) slots[i] = new PerkSlot(i, i < available, i < unlocked, i < s.MaxAvailable);
        return new GearState(
            item.Item.IsNone ? item with { Item = new ItemId(s.Item) } : item,
            new ItemId(s.Item),
            Rarity(s.Rarity) ?? type,
            type,
            s.MaxAvailable is >= 1 and <= MaxEnhancements ? (ItemRarity)(s.MaxAvailable - 1) : type,
            level,
            level - unlocked,
            available,
            unlocked,
            level + available - unlocked,
            (s.State & ItemStateHasGemSlot) != 0,
            durability ? s.Durability : null,
            durability ? s.DurabilityMax : null,
            slots)
        {
            HasEnhancementSlots = perks,
            ShownRarity = Rarity(s.RarityUi) ?? Rarity(s.Rarity) ?? type,
            ShownLevel = s.LevelUi,
            TypeLevelMin = (int)Math.Min(s.TypeLevelMin, int.MaxValue),
            TypeLevelMax = (int)Math.Min(s.TypeLevelMax, int.MaxValue),
            IsEquipped = (s.Flags & ItemsNative.GearEquippedNow) != 0,
        };
    }

    private static ItemRarity? Rarity(byte b) => b <= 4 ? (ItemRarity)b : null;

    /// <summary>The managed result of a native one (pure).</summary>
    internal static GearResult GearResultOf(ItemInstanceRef item, in ItemsNative.GearResult n)
    {
        var status = Enum.IsDefined(typeof(GearStatus), n.Status) ? (GearStatus)n.Status : GearStatus.Refused;
        string message;
        fixed (byte* m = n.Message) message = ItemsNative.Text(m, 160);
        var ran = n.Before.Item != 0;  // a stale or dropped write read nothing
        var changed = GearChanges.None;
        if ((n.Changed & ItemsNative.GearChangedPerks) != 0) changed |= GearChanges.Enhancements;
        if ((n.Changed & ItemsNative.GearChangedLevel) != 0) changed |= GearChanges.Level;
        if ((n.Changed & ItemsNative.GearChangedDurability) != 0) changed |= GearChanges.Durability;
        if ((n.Changed & ItemsNative.GearChangedState) != 0) changed |= GearChanges.DurabilityRefresh;
        if ((n.Changed & ItemsNative.GearChangedDurabilityMax) != 0) changed |= GearChanges.DurabilityMax;
        return new GearResult(status, item, ran ? StateOf(item, in n.Before) : null, ran ? StateOf(item, in n.After) : null,
            status == GearStatus.Done ? changed : GearChanges.None, message, (int)n.Steps, (n.Flags & ItemsNative.GearDryRun) != 0);
    }

    // ---- the capability ----

    private static ItemsNative.TableV8* RequireGearRead(string api)
    {
        var t = V8;
        if (t == null)
            throw new KilnUnavailableException(Caps.GearRead, T == null ? "kiln.items is not loaded" : $"kiln.items v{T->Header.Version} has no gear (update Kiln)");
        // the sigdb row is read live (kiln-items.dll lowers it after a fault); the managed row follows every 25 steps
        if (!Can(Caps.Gear)) throw new KilnUnavailableException(Caps.GearRead, $"needs {Caps.Gear}: {Capabilities.Get(Caps.Gear).ReasonText}");
        if (!Can(Caps.GearRead)) throw new KilnUnavailableException(Caps.GearRead, Capabilities.Get(Caps.GearRead).ReasonText);
        return t;
    }

    private static ItemsNative.TableV8* RequireGearWrite(string api, DurabilityRule durability)
    {
        var t = V8;
        if (t == null)
            throw new KilnUnavailableException(Caps.GearWrite, T == null ? "kiln.items is not loaded" : $"kiln.items v{T->Header.Version} has no gear writes (update Kiln)");
        if (!Can(Caps.Gear)) throw new KilnUnavailableException(Caps.GearWrite, $"needs {Caps.Gear}: {Capabilities.Get(Caps.Gear).ReasonText}");
        if (!Can(Caps.GearWrite)) throw new KilnUnavailableException(Caps.GearWrite, Capabilities.Get(Caps.GearWrite).ReasonText);
        if (GearStatusOf(t) is { Available: false } s) throw new KilnUnavailableException(Caps.GearWrite, s.Detail);
        if (durability != DurabilityRule.Keep && !Can(Caps.GearDurability))
            throw new KilnUnavailableException(Caps.GearDurability, Capabilities.Get(Caps.GearDurability).ReasonText);
        return t;
    }

    internal readonly record struct GearServiceState(bool Available, int Program, bool Replicated, bool Saved, int Pending, string Detail);

    internal static GearServiceState? GearStatusOf(ItemsNative.TableV8* t)
    {
        ItemsNative.GearStatus s = default;
        s.Size = (uint)sizeof(ItemsNative.GearStatus);
        if (t->GetGearStatus(&s) != ItemsNative.Ok) return null;
        return new GearServiceState(s.Available != 0, s.Program, s.Replicated != 0, s.Saved != 0, (int)s.Pending, ItemsNative.Text(s.Detail, 160));
    }

    // ---- submit and results ----

    // A write in flight, held by Kiln.Items itself (owner 0): it completes and is reported (the gear events, the gear
    // watch) even when the mod that asked unloads first; only that mod's task is dropped then (its continuations are the
    // unloaded mod's code).
    private sealed class PendingGear(TaskCompletionSource<GearResult> tcs, ModContext? mod, ItemInstanceRef item, GearChangeSource source, string origin,
        GearKeepIntent keep = GearKeepIntent.None)
    {
        public readonly GearKeepIntent Keep = keep;
        public readonly TaskCompletionSource<GearResult> Tcs = tcs;
        public readonly ModContext? Mod = mod;
        public readonly ItemInstanceRef Item = item;
        public readonly GearChangeSource Source = source;
        public readonly string Origin = origin;
        public bool Retired;
    }

    private static readonly object GearGate = new();
    private static readonly Dictionary<long, PendingGear> GearsInFlight = new();
    private static readonly HashSet<ModContext> GearsWatched = new();
    private static long _nextGear;

    /// <summary>One gear write for a mod (the Set* calls) or an upgrade (<paramref name="source"/>).</summary>
    internal static Task<GearResult> WriteGear(ItemInstanceRef item, GearChange change, GearWriteOptions? options, GearChangeSource source, string api,
        GearKeepIntent keep = GearKeepIntent.None)
    {
        Services.RequireTickThread(api);
        options ??= GearWriteOptions.Default;
        var stamp = StampOf(item, nameof(item));
        var request = GearRequestOf(item, stamp, change, options);
        var t = RequireGearWrite(api, options.Durability);
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var tcs = new TaskCompletionSource<GearResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        long key;
        lock (GearGate)
        {
            key = ++_nextGear;
            GearsInFlight[key] = new PendingGear(tcs, mod, item, source, mod?.Info.Id ?? "kiln", keep);
            if (mod != null && GearsWatched.Add(mod)) mod.Retiring += () => RetireGears(mod);
        }
        GearWatch.Begin(item);
        ulong ticket = 0;
        var rc = t->GearWrite(0, &request, &OnGearDone, (void*)key, &ticket);
        if (rc == ItemsNative.Ok) return tcs.Task;
        lock (GearGate) GearsInFlight.Remove(key);
        GearWatch.End(item);
        if (rc == ItemsNative.InvalidArgument) throw new ArgumentException($"{api}: kiln.items refused the request's arguments");
        var s = GearStatusOf(t);
        if (s is { Available: true, Pending: >= 1024 })
            return Task.FromResult(GearResult.Fail(GearStatus.Refused, item, "1024 gear writes are waiting: try again in a step", options.DryRun));
        throw new KilnUnavailableException(Caps.GearWrite, s?.Detail is { Length: > 0 } d ? d : $"kiln.items refused it (error {rc})");
    }

    private static void RetireGears(ModContext mod)
    {
        lock (GearGate)
        {
            GearsWatched.Remove(mod);
            foreach (var p in GearsInFlight.Values)
                if (p.Mod == mod) p.Retired = true;
        }
    }

    /// <summary>A native result arrived (tick thread, Pre): the gear watch and the events hear of every write that changed
    /// something, the asking mod's task when it is still loaded.</summary>
    internal static void CompleteGear(long key, in ItemsNative.GearResult n)
    {
        PendingGear? p;
        lock (GearGate)
        {
            if (!GearsInFlight.Remove(key, out p)) return;
        }
        var result = GearResultOf(p.Item, in n);
        GearWatch.End(p.Item, result is { Ok: true, DryRun: false } ? result.After : null);
        if (result is { Ok: true, DryRun: false, Before: { } before, After: { } after })
            GearEvents.Changed(p.Item, before, after, p.Source, p.Origin);
        if (p.Keep != GearKeepIntent.None && result is { Ok: true, DryRun: false, After: { } kept })
            result = result with { Kept = GearKeep.AfterWrite(p.Item, p.Keep, kept) };
        if (!p.Retired) p.Tcs.TrySetResult(result);
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnGearDone(ItemsNative.GearResult* r, void* user)
    {
        try
        {
            CompleteGear((long)user, in *r);
        }
        catch (Exception e)
        {
            // never let an exception cross into native code
            try
            {
                Backend.Current.Log(0, LogLevel.Error, $"kiln.items: a gear write's result failed: {e.Message}");
            }
            catch
            {
                // nothing left to say
            }
        }
    }
}
