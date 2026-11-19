// SPDX-License-Identifier: MIT
// The fake kiln.items' gear (kiln_items.h v8 "gear"): every piece of gear the fake creates gets an item entity record
// with the components a gear write reads and writes (PerkContainerNew, Level, Durability, ItemState's derived bytes) and
// a stamp; gear_write runs at the start of the next step with the native service's rules (native/services/items/
// gear_logic.h, ported rule for rule), gear_read reads the record. What the game derives (the shown rarity and level)
// follows one step later, as item_state_init does; max durability is not computed again (a refresh only clears and sets
// the bit), and nothing is ever equipped.
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

internal sealed unsafe partial class ItemsFake
{
    /// <summary>The sigdb feature of the gear functions: a test can switch it off.</summary>
    public const string GearFeature = "items.gear";

    private const int GearSlots = 5, GearMaxLevel = 255, GearMaxPending = 1024;

    /// <summary>One item entity's gear components.</summary>
    internal sealed class GearRec
    {
        public uint Stamp, Item;
        public bool HasPerks, HasLevel, HasDurability;
        public byte Available, Unlocked, MaxAvailable, TypeRarity, RarityUi, LevelUi;
        public uint Level, LevelMax, TypeMin, TypeMax;
        public int Durability, DurabilityMax;
        public ushort State;
    }

    private sealed class GearJob
    {
        public required ulong Ticket, Owner, Step;
        public required ItemsAbi.GearRequest Request;
        public required nint Done, User;
    }

    private static uint _stamps;  // per process, as kiln.items' counter: a stamp names one item entity once

    private readonly Dictionary<uint, GearRec> _gear = [];
    private readonly List<GearJob> _gearPending = [];
    private ulong _gearWrites, _gearRefused;

    /// <summary>Gear made (createPide's rules, docs/re/items.md section 10): enhancement slots rarity + 1 when the type has a
    /// perk in the rarity's slot, none unlocked; the level with its max; full durability; the game's derived bytes.</summary>
    private void NewGear(uint pide, FakeItem item, int level, byte rarity)
    {
        var rec = new GearRec
        {
            Stamp = ++_stamps, Item = item.Id, TypeRarity = (byte)item.Rarity, MaxAvailable = (byte)Math.Clamp(item.PerkSlots, 0, GearSlots),
            TypeMin = (uint)Math.Max(item.LevelMin, 0), TypeMax = (uint)Math.Max(item.LevelMax, 0),
        };
        if (rarity < rec.MaxAvailable)
        {
            rec.HasPerks = true;
            rec.Available = (byte)Math.Min(rarity + 1, GearSlots);
        }
        if (level > 0)
        {
            rec.HasLevel = true;
            rec.Level = rec.LevelMax = (uint)level;
            rec.State |= ItemsAbi.StateHasLevel;
        }
        if (item.Durability > 0)
        {
            rec.HasDurability = true;
            rec.Durability = rec.DurabilityMax = item.Durability;
            rec.State |= ItemsAbi.StateDurabilityInitialized;
        }
        rec.RarityUi = RarityOf(rec);
        rec.LevelUi = (byte)Math.Min(rec.Level, 255);
        lock (_lock) _gear[pide] = rec;
    }

    private void DestroyGear(uint pide)
    {
        lock (_lock) _gear.Remove(pide);
        World.Destroy(pide);
    }

    /// <summary>The stamp of a gear item entity (0: none).</summary>
    public uint StampOf(uint pide)
    {
        lock (_lock) return _gear.TryGetValue(pide, out var g) ? g.Stamp : 0;
    }

    /// <summary>The gear record of an item entity whose stamp matches (0: any), or null.</summary>
    public GearRec? Gear(uint pide, uint stamp)
    {
        lock (_lock) return _gear.TryGetValue(pide, out var g) && (stamp == 0 || g.Stamp == stamp) ? g : null;
    }

    private static byte RarityOf(GearRec g) =>
        g.HasPerks && g.Available is >= 1 and <= GearSlots ? (byte)(g.Available - 1) : g.TypeRarity;

    private static ItemsAbi.GearState StateOf(GearRec g) => new()
    {
        Flags = (g.HasPerks ? ItemsAbi.GearHasPerks : 0) | (g.HasLevel ? ItemsAbi.GearHasLevel : 0) | (g.HasDurability ? ItemsAbi.GearHasDurability : 0) |
                ((g.State & ItemsAbi.StateDurabilityInitialized) != 0 ? ItemsAbi.GearDurabilityInit : 0),
        Item = g.Item, Available = g.HasPerks ? g.Available : (byte)0, Unlocked = g.HasPerks ? g.Unlocked : (byte)0, Rarity = RarityOf(g),
        TypeRarity = g.TypeRarity, MaxAvailable = g.MaxAvailable, RarityUi = g.RarityUi, LevelUi = g.LevelUi,
        Level = g.HasLevel ? g.Level : 0, LevelMax = g.HasLevel ? g.LevelMax : 0, TypeLevelMin = g.TypeMin, TypeLevelMax = g.TypeMax,
        Durability = g.HasDurability ? g.Durability : 0, DurabilityMax = g.HasDurability ? g.DurabilityMax : 0, State = g.State,
    };

    // ---- the table's functions ----

    /// <summary>gear_logic.h request_valid.</summary>
    public static bool GearRequestValid(in ItemsAbi.GearRequest r)
    {
        if (r.Size < (uint)sizeof(ItemsAbi.GearRequest) || r.Entity == 0 || r.Stamp == 0) return false;
        if ((r.Flags & ~ItemsAbi.GearFlags) != 0 || (r.Flags & ItemsAbi.GearOps) == 0) return false;
        if (r.Reserved0 != 0 || r.R0 != 0 || r.R1 != 0 || r.R2 != 0) return false;
        if ((r.Flags & ItemsAbi.GearAvailable) != 0 ? r.Available is < 1 or > GearSlots : r.Available != 0) return false;
        if ((r.Flags & ItemsAbi.GearUnlocked) != 0 ? r.Unlocked > GearSlots : r.Unlocked != 0) return false;
        if ((r.Flags & ItemsAbi.GearLevel) != 0 ? r.Level is < 1 or > GearMaxLevel : r.Level != 0) return false;
        return true;
    }

    private bool GearUsable => !FakeTables.Off(GearFeature) && Program is ItemsAbi.ProgramPending or ItemsAbi.ProgramRunning;

    public int SubmitGear(ulong owner, ItemsAbi.GearRequest request, nint done, nint user, ulong* ticket)
    {
        if (!GearUsable) return FakeTables.Unavailable;
        var job = new GearJob
        {
            Ticket = (ulong)Interlocked.Increment(ref _tickets), Owner = owner, Step = _harness.CurrentStep, Request = request, Done = done, User = user,
        };
        lock (_lock)
        {
            if (_gearPending.Count >= GearMaxPending) return FakeTables.Unavailable;
            _gearPending.Add(job);
        }
        if (ticket != null) *ticket = job.Ticket;
        return FakeTables.Ok;
    }

    public int ReadGear(uint entity, uint stamp, ItemsAbi.GearState* out_)
    {
        lock (_lock)
        {
            if (!_gear.TryGetValue(entity, out var g)) return FakeTables.NotFound;
            if (stamp != 0 && g.Stamp != stamp) return FakeTables.NotFound;
            *out_ = StateOf(g);
            return FakeTables.Ok;
        }
    }

    public void FillGear(ref ItemsAbi.GearStatus s)
    {
        lock (_lock)
        {
            s.Available = GearUsable ? (byte)1 : (byte)0;
            s.Program = (byte)(FakeTables.Off(GearFeature) ? ItemsAbi.ProgramNone : Program);
            s.Replicated = 1;
            s.Saved = 1;
            s.Pending = (uint)_gearPending.Count;
            s.Writes = _gearWrites;
            s.Refused = _gearRefused;
            var detail = FakeTables.Off(GearFeature) ? "the sigdb feature items.gear is off on this build" : GearUsable ? "running" : "waiting for the server world";
            fixed (byte* d = s.Detail) SpawnAbi.CopyText(detail, d, ItemsAbi.MessageMax);
        }
    }

    // A cancel drops the callback, not the write (gear.cpp gear_cancel erases the callback only).
    public bool CancelGear(ulong ticket)
    {
        lock (_lock)
        {
            var job = _gearPending.FirstOrDefault(j => j.Ticket == ticket && j.Done != 0);
            if (job is null) return false;
            job.Done = 0;
            return true;
        }
    }

    public void CancelGearOwner(ulong owner)
    {
        lock (_lock)
            foreach (var job in _gearPending.Where(j => j.Owner == owner)) job.Done = 0;
    }

    // ---- the step ----

    /// <summary>The game's item_state_init and item_durability_init, one step after a write: the shown rarity and level
    /// follow the components, and a cleared DurabilityInitialized is set again (max durability as it was: the kit has no
    /// balancing table). The slots' shown level and rarity (Items.Read) follow too.</summary>
    private void DeriveGear()
    {
        lock (_lock)
        {
            foreach (var g in _gear.Values)
            {
                g.RarityUi = RarityOf(g);
                if (g.HasLevel) g.LevelUi = (byte)Math.Min(g.Level, 255);
                if (g.HasDurability) g.State |= ItemsAbi.StateDurabilityInitialized;
            }
            foreach (var inv in _inventories.Values)
                foreach (var s in inv.Slots)
                    if (s.Pide != 0 && _gear.TryGetValue(s.Pide, out var g))
                    {
                        s.Rarity = g.RarityUi;
                        s.Level = g.LevelUi;
                    }
        }
    }

    /// <summary>kiln_items_gear: every write asked for before this step, in order, then its callback.</summary>
    private void RunGear(ulong step)
    {
        List<GearJob> jobs;
        lock (_lock)
        {
            if (_gearPending.Count == 0) return;
            jobs = [.. _gearPending];
            _gearPending.Clear();
        }
        foreach (var job in jobs)
        {
            var q = job.Request;
            var r = new ItemsAbi.GearResult
            {
                Size = (uint)sizeof(ItemsAbi.GearResult), Ticket = job.Ticket, Entity = q.Entity, Stamp = q.Stamp, Flags = q.Flags,
                Steps = (uint)Math.Max(0L, (long)step - (long)job.Step - 1), Step = step,
            };
            string why;
            lock (_lock)
            {
                if (!GearUsable)
                {
                    r.Status = ItemsAbi.GearUnavailable;
                    why = "gear writes were switched off since the request was accepted";
                }
                else if (!_gear.TryGetValue(q.Entity, out var g) || g.Stamp != q.Stamp || (q.Item != 0 && g.Item != q.Item))
                {
                    r.Status = ItemsAbi.GearStale;
                    why = "the entity is not the stamped item entity any more";
                }
                else
                {
                    var before = StateOf(g);
                    r.Before = before;
                    var p = Plan(q, before);
                    r.Status = p.Status;
                    r.After = p.After;
                    r.Changed = p.Changed;
                    why = p.Why;
                    if (p.Status == ItemsAbi.GearDone && (q.Flags & ItemsAbi.GearDryRun) == 0 && p.Changed != 0)
                    {
                        g.Available = p.After.Available;
                        g.Unlocked = p.After.Unlocked;
                        if (g.HasLevel)
                        {
                            g.Level = p.After.Level;
                            g.LevelMax = p.After.LevelMax;
                        }
                        if (g.HasDurability) g.Durability = p.After.Durability;
                        g.State = p.After.State;
                        _gearWrites++;
                    }
                }
                if (r.Status != ItemsAbi.GearDone) _gearRefused++;
            }
            SpawnAbi.CopyText(r.Status == ItemsAbi.GearDone ? "" : why, r.Message, ItemsAbi.MessageMax);
            if (job.Done != 0) ((delegate* unmanaged[Cdecl]<ItemsAbi.GearResult*, void*, void>)job.Done)(&r, (void*)job.User);
        }
    }

    internal readonly record struct GearPlan(int Status, string Why, ItemsAbi.GearState After, uint Changed);

    /// <summary>gear_logic.h plan(), rule for rule (no equipment in the kit: NOT_EQUIPPED never refuses).</summary>
    internal static GearPlan Plan(in ItemsAbi.GearRequest r, in ItemsAbi.GearState before)
    {
        var b0 = before;
        GearPlan Refuse(int status, string why) => new(status, why, b0, 0);
        var after = before;
        var perks = (before.Flags & ItemsAbi.GearHasPerks) != 0;
        var level = (before.Flags & ItemsAbi.GearHasLevel) != 0;
        var durability = (before.Flags & ItemsAbi.GearHasDurability) != 0;
        if ((r.Flags & ItemsAbi.GearNotEquipped) != 0 && (before.Flags & ItemsAbi.GearEquippedNow) != 0) return Refuse(ItemsAbi.GearEquipped, "the item is equipped");
        if ((r.Flags & (ItemsAbi.GearAvailable | ItemsAbi.GearUnlocked)) != 0 && !perks)
            return Refuse(ItemsAbi.GearNotGear, "the item has no enhancement slots (no PerkContainerNew)");
        byte avail = before.Available, unl = before.Unlocked;
        if ((r.Flags & ItemsAbi.GearAvailable) != 0)
        {
            if (r.Available > before.Available && r.Available > before.MaxAvailable && (r.Flags & ItemsAbi.GearForceSlot) == 0)
                return Refuse(ItemsAbi.GearNoSlot, "the item's type has no perk in that enhancement slot");
            avail = r.Available;
            if (unl > avail) unl = avail;
        }
        if ((r.Flags & ItemsAbi.GearUnlocked) != 0)
        {
            if (r.Unlocked > avail) return Refuse(ItemsAbi.GearOutOfRange, "more enhancements unlocked than the item has slots");
            unl = r.Unlocked;
        }
        long delta = unl - (long)before.Unlocked, newLevel = before.Level;
        if ((r.Flags & ItemsAbi.GearLevel) != 0)
        {
            if (!level) return Refuse(ItemsAbi.GearNotGear, "the item has no Level");
            var b = (long)r.Level - unl;
            if (b < 1 || b < before.TypeLevelMin || b > before.TypeLevelMax)
                return Refuse(ItemsAbi.GearOutOfRange, "the level's base is outside the type's level range");
            newLevel = r.Level;
        }
        else if (delta != 0)
        {
            if (!level) return Refuse(ItemsAbi.GearNotGear, "the item has no Level for its unlocked enhancements");
            newLevel += delta;
            if (newLevel is < 1 or > GearMaxLevel) return Refuse(ItemsAbi.GearOutOfRange, "the level would leave 1..255");
        }
        uint changed = 0;
        after.Available = avail;
        after.Unlocked = unl;
        after.Rarity = perks && avail is >= 1 and <= GearSlots ? (byte)(avail - 1) : before.TypeRarity;
        if (perks && (avail != before.Available || unl != before.Unlocked)) changed |= ItemsAbi.GearChangedPerks;
        if (level)
        {
            after.Level = (uint)newLevel;
            after.LevelMax = after.Level > before.LevelMax ? after.Level : before.LevelMax;
            if (after.Level != before.Level || after.LevelMax != before.LevelMax) changed |= ItemsAbi.GearChangedLevel;
        }
        if (durability && (r.Flags & (ItemsAbi.GearDurabilityRefresh | ItemsAbi.GearDurabilityRepair)) != 0)
        {
            if ((r.Flags & ItemsAbi.GearDurabilityRepair) != 0 && before.Durability != before.DurabilityMax)
            {
                after.Durability = before.DurabilityMax;
                changed |= ItemsAbi.GearChangedDurability;
            }
            if ((before.State & ItemsAbi.StateDurabilityInitialized) != 0)
            {
                after.State = (ushort)(before.State & ~ItemsAbi.StateDurabilityInitialized);
                after.Flags &= ~ItemsAbi.GearDurabilityInit;
                changed |= ItemsAbi.GearChangedState;
            }
        }
        return new GearPlan(ItemsAbi.GearDone, "", after, changed);
    }

    // ---- the test's hand ----

    /// <summary>As if the game had changed the item's gear (a player's unlock at the workbench, the free +1 / -1): no
    /// write of Kiln's, no event; the derived bytes follow at the next step. Null leaves a value as it is.</summary>
    public void SetGear(uint pide, uint stamp, int? enhancements, int? unlocked, int? level, int? durability)
    {
        lock (_lock)
        {
            if (Gear(pide, stamp) is not { } g) throw new ArgumentException($"entity {pide} is no gear item entity with stamp {stamp} in the fake", nameof(pide));
            if (enhancements is { } a)
            {
                if (a is < 0 or > GearSlots) throw new ArgumentOutOfRangeException(nameof(enhancements), a, "0 to 5 enhancement slots");
                g.HasPerks = a > 0;
                g.Available = (byte)a;
                if (g.Unlocked > a) g.Unlocked = (byte)a;
            }
            if (unlocked is { } u)
            {
                if (u < 0 || u > g.Available) throw new ArgumentOutOfRangeException(nameof(unlocked), u, $"0 to the item's {g.Available} slots");
                g.Unlocked = (byte)u;
            }
            if (level is { } l)
            {
                if (l is < 1 or > GearMaxLevel) throw new ArgumentOutOfRangeException(nameof(level), l, "a level is 1 to 255");
                g.HasLevel = true;
                g.Level = (uint)l;
                if (g.LevelMax < g.Level) g.LevelMax = g.Level;
            }
            if (durability is { } d)
            {
                if (!g.HasDurability) throw new ArgumentException("the item has no durability", nameof(durability));
                g.Durability = Math.Clamp(d, 0, g.DurabilityMax);
            }
        }
    }
}
