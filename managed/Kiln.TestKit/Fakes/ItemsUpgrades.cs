// SPDX-License-Identifier: MIT
// The fake kiln.items' upgrades (kiln_items.h v8 "upgrades", package P3): gear_upgrade runs at the start of the next step,
// after the gear writes, with the native service's rules (native/services/items/upgrade_logic.h and gear.cpp's upgrade,
// ported rule for rule): the item checked by its stamp, the write planned (a refusal takes nothing), the payers checked
// (the player, or a container: an entity with an inventory that is no other player), each cost counted in the payers'
// plain stacks and split among them in order, then taken and the write made together, all or nothing.
namespace Kiln.TestKit;

internal sealed unsafe partial class ItemsFake
{
    private sealed class UpgradeJob
    {
        public required ulong Ticket, Owner, Step;
        public required ItemsAbi.UpgradeRequest Request;
        public required nint Done, User;
    }

    private readonly List<UpgradeJob> _upgradePending = [];

    /// <summary>upgrade_logic.h request_valid.</summary>
    public static bool UpgradeRequestValid(in ItemsAbi.UpgradeRequest r)
    {
        if (r.Size < (uint)sizeof(ItemsAbi.UpgradeRequest) || r.Player == 0 || (r.Flags & ~(ItemsAbi.UpgradeWrite | ItemsAbi.UpgradeDryRun)) != 0 || r.ExpectAvailable > GearSlots) return false;
        if (r.CostCount > ItemsAbi.UpgradeMaxCosts || r.PayerCount > ItemsAbi.UpgradeMaxPayers || (r.CostCount > 0 && r.PayerCount == 0)) return false;
        var g = r.Gear;
        if (g.Size < (uint)sizeof(ItemsAbi.GearRequest) || g.Entity == 0 || g.Stamp == 0) return false;
        if ((r.Flags & ItemsAbi.UpgradeWrite) != 0)
        {
            if (!GearRequestValid(g) || (g.Flags & ItemsAbi.GearDryRun) != 0) return false;
        }
        else if (g.Flags != 0 || g.Available != 0 || g.Unlocked != 0 || g.Level != 0 || g.Reserved0 != 0 || g.DurabilityMax != 0 || g.R0 != 0 || g.R1 != 0)
        {
            return false;
        }
        for (var i = 0; i < ItemsAbi.UpgradeMaxCosts; i++)
        {
            uint item = r.Costs[2 * i], count = r.Costs[2 * i + 1];
            if (i >= r.CostCount)
            {
                if (item != 0 || count != 0) return false;
                continue;
            }
            if (item == 0 || count is < 1 or > ItemsAbi.UpgradeMaxCount) return false;
            for (var k = 0; k < i; k++)
                if (r.Costs[2 * k] == item) return false;
        }
        for (var i = 0; i < ItemsAbi.UpgradeMaxPayers; i++)
        {
            var p = r.Payers[i];
            if (i >= r.PayerCount)
            {
                if (p != 0) return false;
                continue;
            }
            if (p == 0) return false;
            for (var k = 0; k < i; k++)
                if (r.Payers[k] == p) return false;
        }
        return true;
    }

    public int SubmitUpgrade(ulong owner, ItemsAbi.UpgradeRequest request, nint done, nint user, ulong* ticket)
    {
        if (!GearUsable || FakeTables.Off(TransactionFeature)) return FakeTables.Unavailable;
        var job = new UpgradeJob
        {
            Ticket = (ulong)Interlocked.Increment(ref _tickets), Owner = owner, Step = _harness.CurrentStep, Request = request, Done = done, User = user,
        };
        lock (_lock)
        {
            if (_gearPending.Count + _upgradePending.Count >= GearMaxPending) return FakeTables.Unavailable;
            _upgradePending.Add(job);
        }
        if (ticket != null) *ticket = job.Ticket;
        return FakeTables.Ok;
    }

    public bool CancelUpgrade(ulong ticket)
    {
        lock (_lock)
        {
            var job = _upgradePending.FirstOrDefault(j => j.Ticket == ticket && j.Done != 0);
            if (job is null) return false;
            job.Done = 0;
            return true;
        }
    }

    public void CancelUpgradeOwner(ulong owner)
    {
        lock (_lock)
            foreach (var job in _upgradePending.Where(j => j.Owner == owner)) job.Done = 0;
    }

    /// <summary>kiln_items_gear's upgrades: every one asked for before this step, in order, then its callback.</summary>
    private void RunUpgrades(ulong step)
    {
        List<UpgradeJob> jobs;
        lock (_lock)
        {
            if (_upgradePending.Count == 0) return;
            jobs = [.. _upgradePending];
            _upgradePending.Clear();
        }
        foreach (var job in jobs)
        {
            var q = job.Request;
            var r = new ItemsAbi.UpgradeResult { Size = (uint)sizeof(ItemsAbi.UpgradeResult), Ticket = job.Ticket, Flags = q.Flags, CostCount = q.CostCount };
            r.Gear = new ItemsAbi.GearResult
            {
                Size = (uint)sizeof(ItemsAbi.GearResult), Ticket = job.Ticket, Entity = q.Gear.Entity, Stamp = q.Gear.Stamp, Flags = q.Gear.Flags,
                Steps = (uint)Math.Max(0L, (long)step - (long)job.Step - 1), Step = step,
            };
            string why;
            lock (_lock)
            {
                (r.Status, why) = RunUpgrade(q, ref r);
                if (r.Status != ItemsAbi.GearDone) _gearRefused++;
            }
            r.Gear.Status = r.Status <= ItemsAbi.GearRefused ? r.Status : ItemsAbi.GearRefused;
            SpawnAbi.CopyText(why, r.Gear.Message, ItemsAbi.MessageMax);
            if (job.Done != 0) ((delegate* unmanaged[Cdecl]<ItemsAbi.UpgradeResult*, void*, void>)job.Done)(&r, (void*)job.User);
        }
    }

    // _lock held
    private (int Status, string Why) RunUpgrade(in ItemsAbi.UpgradeRequest q, ref ItemsAbi.UpgradeResult r)
    {
        if (!GearUsable || FakeTables.Off(TransactionFeature)) return (ItemsAbi.GearUnavailable, "gear writes were switched off since the request was accepted");
        if (!_gear.TryGetValue(q.Gear.Entity, out var g) || g.Stamp != q.Gear.Stamp || (q.Gear.Item != 0 && g.Item != q.Gear.Item))
            return (ItemsAbi.GearStale, "the entity is not the stamped item entity any more");
        var before = StateOf(g);
        r.Gear.Before = before;
        r.Gear.After = before;
        if (q.ExpectAvailable != 0 && (!g.HasPerks || g.Available != q.ExpectAvailable))
            return (ItemsAbi.UpgradeChanged, $"the item has {before.Available} enhancement slots now, not the {q.ExpectAvailable} the upgrade was priced for");
        var write = (q.Flags & ItemsAbi.UpgradeWrite) != 0;
        var dry = (q.Flags & ItemsAbi.UpgradeDryRun) != 0;
        var plan = new GearPlan(ItemsAbi.GearDone, "", before, 0);
        if (write)
        {
            plan = Plan(q.Gear, before);
            if (plan.Status != ItemsAbi.GearDone) return (plan.Status, plan.Why);
        }
        var payers = new Inv[q.PayerCount];
        for (var i = 0; i < q.PayerCount; i++)
        {
            var p = q.Payers[i];
            if (Of(p) is not { } inv) return (ItemsAbi.UpgradeBadPayer, $"payer {p} has no initialized inventory");
            if (p != q.Player && p is >= 1 and <= 1023) return (ItemsAbi.UpgradeBadPayer, $"payer {p} is another player or an NPC, not a container");
            payers[i] = inv;
        }
        var takes = new int[q.CostCount, q.PayerCount];
        for (var c = 0; c < q.CostCount; c++)
        {
            var id = q.Costs[2 * c];
            var count = (int)q.Costs[2 * c + 1];
            if (Item(id) is not { } info) return (ItemsAbi.GearRefused, $"cost item 0x{id:x8} is not in the item registry");
            if (info.Gear) return (ItemsAbi.GearRefused, $"cost item 0x{id:x8} has an item entity of its own (gear, a gem): not a cost");
            long have = 0, left = count;
            for (var p = 0; p < q.PayerCount; p++)
            {
                var n = payers[p].Slots.Where(s => s.Count > 0 && s.Item == id && s.Pide == 0).Sum(s => (long)s.Count);
                have += n;
                var t = (int)Math.Min(n, left);
                takes[c, p] = t;
                left -= t;
            }
            r.Have[c] = (uint)Math.Min(have, uint.MaxValue);
            if (left > 0) return (ItemsAbi.UpgradeCantPay, $"the payers hold {have} of the {count} of item 0x{id:x8} it costs");
        }
        if (dry)
        {
            for (var c = 0; c < q.CostCount; c++) r.Paid[c] = q.Costs[2 * c + 1];
            r.Gear.After = plan.After;
            r.Gear.Changed = plan.Changed;
            return (ItemsAbi.GearDone, "");
        }
        // one transaction: the costs out of the payers' stacks, then the write
        for (var c = 0; c < q.CostCount; c++)
        {
            var id = q.Costs[2 * c];
            for (var p = 0; p < q.PayerCount; p++)
            {
                var left = takes[c, p];
                foreach (var s in payers[p].Slots)
                {
                    if (left == 0) break;
                    if (s.Count == 0 || s.Item != id || s.Pide != 0) continue;
                    var took = Math.Min(left, s.Count);
                    s.Count -= took;
                    left -= took;
                    if (s.Count == 0) s.Item = 0;
                }
                if (q.Payers[p] == q.Player) r.PaidFromPlayer += (uint)takes[c, p];
            }
            r.Paid[c] = q.Costs[2 * c + 1];
        }
        if (write && plan.Changed != 0)
        {
            g.Available = plan.After.Available;
            g.Unlocked = plan.After.Unlocked;
            if (g.HasLevel)
            {
                g.Level = plan.After.Level;
                g.LevelMax = plan.After.LevelMax;
            }
            if (g.HasDurability) g.Durability = plan.After.Durability;
            g.State = plan.After.State;
            _gearWrites++;
        }
        r.Gear.After = StateOf(g);
        r.Gear.Changed = write ? plan.Changed : 0;
        return (ItemsAbi.GearDone, "");
    }
}
