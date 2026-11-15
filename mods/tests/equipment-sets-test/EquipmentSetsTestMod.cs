// SPDX-License-Identifier: MIT
// equipment-sets-test: kiln.items v6 equipment sets (plan 10-equipment-sets, chapter 10 §6) on a spawned 1_Player
// character in a lab (no client: the game's own player template has PlayerInput, SlotSelection and an inventory, so
// kiln_items_equip iterates it like a joined player's; it is a stand-in for a player, so these rows prove Kiln's
// machinery on the game's real inventory transaction, not what a player's client shows). Every row is one PASS or
// FAIL line named after the chapter's table (SET-CAPTURE ... SET-INVARIANT). kiln/data/equipment-sets-test/matrix (with
// kiln/data/kiln.items/sets-matrix) runs the rollback matrix instead: one fresh character per form of a set. Faults are injected into real runs through
// Items.LabEquipSet (kiln/data/kiln.items/sets-lab): a refusal of the game's own equip at a chosen step, the commit's own
// rollback bit, a faked mismatch after the commit (last: it switches sets off until the server restarts). After every
// refusal, dry run and commit rollback kiln.items itself compares the canonical snapshot (InventorySetup, every
// Inventory, SlotSelection, ItemState, OwnerRelationship) and would report InvariantFailure: a Refused or Simulated
// status is that comparison passing; the test compares its own full read of the inventory as well. With
// kiln/control/systems.parallel the run repeats with the game's programs forced onto its workers (the native log line
// of each set says the thread). The character is destroyed at the end.
using System.Text.Json;
using Kiln;
using Kiln.Testing;

namespace EquipmentSetsTest;

public sealed class EquipmentSetsTestMod : Mod
{
    private int _pass, _fail;
    private bool _stop;
    private readonly List<EquipmentSetChangedEvent> _changed = [];
    private readonly List<EquipmentSetFaultedEvent> _faulted = [];
    private readonly List<PlayerEquippingEvent> _deciding = [];
    private bool _cancelNext;

    public override void OnLoad() => _ = RunAsync();

    public override void OnUnload() => _stop = true;

    private void Check(bool ok, string what)
    {
        if (ok) _pass++;
        else _fail++;
        Log.Info($"{(ok ? "PASS" : "FAIL")} {what}");
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n && !_stop; i++) await Context.Scheduler.NextTick();
    }

    private async Task RunAsync()
    {
        try
        {
            await Run();
        }
        catch (Exception e)
        {
            Check(false, $"test aborted: {e}");
        }
        Log.Info($"equipment-sets-test done: {_pass} PASS, {_fail} FAIL");
    }

    // ---- fixtures ----
    private static ItemId Id(string debugName) => Items.Lookup(debugName)?.Id ?? throw new InvalidOperationException($"no item {debugName} in this build");

    // Every stack with its count, item entity and equipped flag, and the inventory links: a refusal must leave all of it.
    private static string Gear(EntityId p) =>
        Items.Read(p) is { } c
            ? $"links {string.Join(",", c.Inventories.Select(i => $"{i.Inventory}/{i.Category}"))} slots {c.GenericSlots}/{c.AvailableSlots}; " +
              string.Join(" ", c.Slots.Where(s => !s.IsEmpty).Select(s => $"{s.Slot}={s.Item.Value:x8}x{s.Count}#{s.ItemEntity}{(s.IsEquipped ? "*" : "")}"))
            : "unreadable";

    private static InventorySlot? Loose(EntityId p, ItemId item) => Items.Read(p)?.Slots.FirstOrDefault(s => s.Item == item && !s.IsEquipped && !s.IsEmpty);

    private static EquipmentSet Set(EquipmentSetScope scope, params (EquipmentSlot Slot, EquipmentSetSelection Selection)[] e) =>
        new(scope, e.Select(x => new EquipmentSetEntry(x.Slot, x.Selection)).ToArray());

    private static string Show(EquipmentSetResult r) =>
        $"{r.Status} ok={r.Ok} dry={r.DryRun} committed={r.Committed} mutated={r.MayHaveMutated} failed={r.FailedOperation?.ToString() ?? "-"} " +
        $"error=0x{r.GameError:x2} decision={(r.Decision is { } d ? $"{d.WorldEpoch}:{d.Request}" : "none")} changes={r.Changes.Count} ticket={r.Ticket}" +
        (r.Reason is { } why ? $": {why}" : "");

    private int ChangedFor(ulong ticket) => _changed.Count(e => e.Ticket == ticket);
    private int DecisionsFor(ulong ticket) => _deciding.Count(e => e.Change is SetEquipmentChange s && s.ItemsTicket == ticket);

    // ---- the rollback matrix (kiln/data/equipment-sets-test/matrix with kiln/data/kiln.items/sets-matrix): one fresh
    // character per form, one set each; a form whose commit is not exactly as asked must be refused by the planner ----
    private async Task<EntityId> NewPlayer(WorldPosition at)
    {
        var p = await Spawn.Entity("1_Player", at, 0, new SpawnOptions { Tag = "equipment-sets-test", Flags = SpawnFlags.Transient });
        await Steps(40);
        foreach (var name in (string[])["Ring_T3_Ring_of_Light", "Ring_T6_Ring_of_Shock", "Ring_T1_Ring_of_Mana", "Armor_T1_Warrior_DPS_Helmet",
                     "Armor_T1_Ranger_Archer_DPS_Helmet"])
            await Items.Give(p, Id(name), 1);
        await Items.Give(p, Id("Ammo_T1_Arrow_Flint_lvl5"), 20);
        await Items.Give(p, Id("Ammo_T1_Arrow_Scrap_lvl10"), 15);
        await Steps(6);
        return p;
    }

    private static bool Holds(EntityId p, EquipmentSet set)
    {
        var now = Items.CaptureSet(p).Set;
        if (now is null) return false;
        foreach (var e in set.Entries)
        {
            var have = now.Entries.First(x => x.Slot == e.Slot).Selection;
            var ok = e.Selection switch
            {
                EmptyEquipment => have is EmptyEquipment,
                InstanceEquipment i => have is InstanceEquipment h && h.Item == i.Item && Items.Equipped(p, e.Slot) == i.Item,
                StackEquipment st => have is StackEquipment h && h.Item == st.Item && h.ExpectedCount == st.ExpectedCount,
                _ => false,
            };
            if (!ok) return false;
        }
        return true;
    }

    private async Task Matrix(WorldPosition at)
    {
        var helmetA = Id("Armor_T1_Warrior_DPS_Helmet");
        var helmetB = Id("Armor_T1_Ranger_Archer_DPS_Helmet");
        var light = Id("Ring_T3_Ring_of_Light");
        var shock = Id("Ring_T6_Ring_of_Shock");
        var flint = Id("Ammo_T1_Arrow_Flint_lvl5");
        var scrap = Id("Ammo_T1_Arrow_Scrap_lvl10");
        EquipmentSetSelection Pick(EntityId p, ItemId item) =>
            Loose(p, item) is { } s ? (s.Ref.IsNone ? new StackEquipment(s.Item, s.Slot, s.Count) : new InstanceEquipment(s.Ref)) : new EmptyEquipment();
        async Task Wear(EntityId p, ItemId item, EquipmentSlot slot)
        {
            if (Loose(p, item) is { } s) await Items.Equip(p, s.Slot, slot);
            await Steps(1);
        }
        var forms = new (string Name, (ItemId Item, EquipmentSlot Slot)[] Pre, Func<EntityId, (EquipmentSlot, EquipmentSetSelection)[]> Entries)[]
        {
            ("three into empty slots", [], p => [(EquipmentSlot.ArmourHead, Pick(p, helmetA)), (EquipmentSlot.Ring1, Pick(p, light)), (EquipmentSlot.Arrow, Pick(p, flint))]),
            ("one swap", [(helmetA, EquipmentSlot.ArmourHead)], p => [(EquipmentSlot.ArmourHead, Pick(p, helmetB))]),
            ("a swap, then an equip into an empty slot", [(helmetA, EquipmentSlot.ArmourHead)], p => [(EquipmentSlot.ArmourHead, Pick(p, helmetB)), (EquipmentSlot.Ring1, Pick(p, light))]),
            ("an equip into an empty slot, then a swap", [(light, EquipmentSlot.Ring1)], p => [(EquipmentSlot.ArmourHead, Pick(p, helmetA)), (EquipmentSlot.Ring1, Pick(p, shock))]),
            ("two swaps", [(helmetA, EquipmentSlot.ArmourHead), (light, EquipmentSlot.Ring1)], p => [(EquipmentSlot.ArmourHead, Pick(p, helmetB)), (EquipmentSlot.Ring1, Pick(p, shock))]),
            ("a swap, then arrows into the empty arrow slot", [(helmetA, EquipmentSlot.ArmourHead)], p => [(EquipmentSlot.ArmourHead, Pick(p, helmetB)), (EquipmentSlot.Arrow, Pick(p, flint))]),
            ("an arrow swap alone", [(flint, EquipmentSlot.Arrow)], p => [(EquipmentSlot.Arrow, Pick(p, scrap))]),
            ("an equip into an empty slot, then an arrow swap", [(flint, EquipmentSlot.Arrow)], p => [(EquipmentSlot.Ring1, Pick(p, light)), (EquipmentSlot.Arrow, Pick(p, scrap))]),
            ("a swap and a removal", [(helmetA, EquipmentSlot.ArmourHead), (light, EquipmentSlot.Ring1)], p => [(EquipmentSlot.ArmourHead, Pick(p, helmetB)), (EquipmentSlot.Ring1, new EmptyEquipment())]),
            ("a removal and an equip into an empty slot", [(helmetA, EquipmentSlot.ArmourHead)], p => [(EquipmentSlot.ArmourHead, new EmptyEquipment()), (EquipmentSlot.Ring1, Pick(p, light))]),
            ("two removals", [(helmetA, EquipmentSlot.ArmourHead), (light, EquipmentSlot.Ring1)], p => [(EquipmentSlot.ArmourHead, new EmptyEquipment()), (EquipmentSlot.Ring1, new EmptyEquipment())]),
            ("a removal, then the removed ring into another slot", [(light, EquipmentSlot.Ring0)], p => [(EquipmentSlot.Ring0, new EmptyEquipment()), (EquipmentSlot.Ring1, new InstanceEquipment(Items.Equipped(p, EquipmentSlot.Ring0)))]),
            ("a removal, then a swap", [(helmetA, EquipmentSlot.ArmourHead), (light, EquipmentSlot.Ring1)], p => [(EquipmentSlot.Ring1, new EmptyEquipment()), (EquipmentSlot.ArmourHead, Pick(p, helmetB))]),
            ("two equips into empty slots, then a swap, then arrows", [(helmetA, EquipmentSlot.ArmourHead)],
                p => [(EquipmentSlot.ArmourHead, Pick(p, helmetB)), (EquipmentSlot.Ring0, Pick(p, light)), (EquipmentSlot.Ring1, Pick(p, shock)), (EquipmentSlot.Arrow, Pick(p, flint))]),
            ("two worn rings swapped (a cycle)", [(light, EquipmentSlot.Ring0), (shock, EquipmentSlot.Ring1)],
                p => [(EquipmentSlot.Ring0, new InstanceEquipment(Items.Equipped(p, EquipmentSlot.Ring1))), (EquipmentSlot.Ring1, new InstanceEquipment(Items.Equipped(p, EquipmentSlot.Ring0)))]),
        };
        foreach (var (name, pre, entries) in forms)
        {
            if (_stop) return;
            var p = await NewPlayer(at);
            try
            {
                foreach (var (item, slot) in pre) await Wear(p, item, slot);
                var scope = Items.CaptureSet(p).Set!.Scope;
                var set = new EquipmentSet(scope, entries(p).Select(e => new EquipmentSetEntry(e.Item1, e.Item2)).ToArray());
                var before = Gear(p);
                var plan = Items.PlanSet(p, set);
                plan.Plan?.Dispose();
                var r = await Items.EquipSet(p, set);
                await Steps(1);
                var held = r.Status == EquipmentSetStatus.Done && Holds(p, set);
                Log.Info($"equipment-sets-test: FORM {name}: plan {(plan.Ok ? $"{plan.Plan!.Steps.Count} step(s)" : plan.Reason)}; {Show(r)}; asked and held {held}; " +
                         $"before {before}; after {Gear(p)}");
                Check(held || r.Status == EquipmentSetStatus.Refused && !r.Committed && Gear(p) == before,
                    $"FORM {name}: {(held ? "committed exactly as asked" : r.Status == EquipmentSetStatus.Refused ? "refused before mutation" : "NOT as asked")} [{Show(r)}]");
            }
            finally
            {
                Spawn.Destroy(p);
                await Steps(3);
            }
        }
    }

    private async Task Run()
    {
        for (var i = 0; i < 25 * 120 && !_stop; i++)
        {
            if (Items.Available && Items.Status() is { Transactions: true, Catalog: true } && Capabilities.Get("items.equip_sets").State != CapabilityState.Pending) break;
            await Steps(1);
        }
        Check(Capabilities.Is("items.equip_sets"), $"items.equip_sets is available ({Capabilities.Get("items.equip_sets").State}: {Capabilities.Why("items.equip_sets")})");
        if (!Capabilities.Is("items.equip_sets")) return;
        var at = new WorldPosition(3845, 835, 1480);
        foreach (var a in World.Find("BaseIdComponent"))
            if (World.Position(a) is { } q)
            {
                at = new WorldPosition(q.X + 45, q.Y + 3, q.Z + 45);
                break;
            }
        if (File.Exists(Path.Combine(Context.DataDirectory, "matrix")))
        {
            if (LabSystems.Enabled) LabSystems.ForceParallel(LabSystems.MaxSteps);
            try
            {
                await Matrix(at);
            }
            finally
            {
                if (LabSystems.Enabled) LabSystems.StopParallel();
            }
            return;
        }
        var p = await Spawn.Entity("1_Player", at, 0, new SpawnOptions { Tag = "equipment-sets-test", Flags = SpawnFlags.Transient });
        await Steps(40);
        Check(World.Has(p, "PlayerInput") && Items.Read(p) is { Initialized: true }, $"a spawned player character {p} with PlayerInput and an initialized inventory");
        using var a1 = Events.On<EquipmentSetChangedEvent>(e => _changed.Add(e));
        using var a2 = Events.On<EquipmentSetFaultedEvent>(e => _faulted.Add(e));
        IDisposable? decide = null;
        if (Capabilities.Is("events.equip"))
            decide = Events.On<PlayerEquippingEvent>(e =>
            {
                if (e.Change is not SetEquipmentChange) return;
                _deciding.Add(e);
                if (_cancelNext) e.Cancel();
            });
        else Log.Info($"equipment-sets-test: events.equip is off ({Capabilities.Why("events.equip")}): no PlayerEquippingEvent handler");
        try
        {
            foreach (var name in (string[])["Ring_T3_Ring_of_Light", "Ring_T6_Ring_of_Shock", "Ring_T1_Ring_of_Mana", "Armor_T1_Warrior_DPS_Helmet",
                         "Armor_T1_Ranger_Archer_DPS_Helmet", "Armor_T1_Warrior_DPS_Chest"])
                await Items.Give(p, Id(name), 1);
            await Items.Give(p, Id("Ammo_T1_Arrow_Flint_lvl5"), 20);
            await Items.Give(p, Id("Ammo_T1_Arrow_Scrap_lvl10"), 15);
            await Steps(6);
            Log.Info($"equipment-sets-test: player {p} after the gives: {Gear(p)}");
            if (LabSystems.Enabled)
            {
                LabSystems.ForceParallel(LabSystems.MaxSteps);
                await Steps(2);
                Log.Info($"equipment-sets-test: the game's programs forced onto its workers (players mask 0x{LabSystems.PlayersMask:x4})");
            }
            else Log.Info("equipment-sets-test: the lab parallel switch is off (no kiln/control/systems.parallel): sets run where the lab's program runs");
            try
            {
                await Rows(p, decide != null);
            }
            finally
            {
                if (LabSystems.Enabled) LabSystems.StopParallel();
            }
        }
        finally
        {
            decide?.Dispose();
            Spawn.Destroy(p);
            await Steps(3);
        }
    }

    private async Task Rows(EntityId p, bool deciding)
    {
        var ringLight = Id("Ring_T3_Ring_of_Light");
        var ringShock = Id("Ring_T6_Ring_of_Shock");
        var ringMana = Id("Ring_T1_Ring_of_Mana");
        var helmetA = Id("Armor_T1_Warrior_DPS_Helmet");
        var helmetB = Id("Armor_T1_Ranger_Archer_DPS_Helmet");
        var flint = Id("Ammo_T1_Arrow_Flint_lvl5");
        var scrap = Id("Ammo_T1_Arrow_Scrap_lvl10");

        // ---- SET-CAPTURE ----
        var cap = Items.CaptureSet(p);
        var scope = cap.Set?.Scope;
        Check(cap is { Ok: true, Set: { } } && cap.Set.Entries.Count == 20 && cap.Set.Entries.Select(e => e.Slot).Distinct().Count() == 20 &&
              scope is { WorldEpoch: > 0 } && scope.ProcessSession != Guid.Empty && scope.Player.Entity == p.Value &&
              cap.Set.Entries.All(e => e.Selection is not InstanceEquipment i || Items.Equipped(p, e.Slot) == i.Item),
            $"SET-CAPTURE: 20 unique slots with explicit empties, scope {scope?.ProcessSession}/{scope?.WorldEpoch}/{scope?.Player}, " +
            $"{cap.Set?.Entries.Count(e => e.Selection is EmptyEquipment)} empty ({cap.Reason})");
        if (cap.Set is not { } captured || scope is null) return;

        // ---- SET-EMPTY / NOCHANGE ----
        var gear = Gear(p);
        var none = await Items.EquipSet(p, captured with { Entries = [] });
        var same = await Items.EquipSet(p, captured);
        var emptySlot = captured.Entries.FirstOrDefault(e => e.Selection is EmptyEquipment)?.Slot ?? EquipmentSlot.Glider;
        var already = await Items.EquipSet(p, Set(scope, (emptySlot, new EmptyEquipment())));
        Check(none is { Status: EquipmentSetStatus.NoChange, Ok: true, Decision: null, Committed: false } &&
              same is { Status: EquipmentSetStatus.NoChange, Decision: null } && already is { Status: EquipmentSetStatus.NoChange, Decision: null } &&
              ChangedFor(none.Ticket) + ChangedFor(same.Ticket) + ChangedFor(already.Ticket) == 0 &&
              DecisionsFor(none.Ticket) + DecisionsFor(same.Ticket) + DecisionsFor(already.Ticket) == 0 && Gear(p) == gear,
            $"SET-EMPTY/NOCHANGE: an empty list, the captured set and an already-empty {emptySlot}: NoChange, no decision, no event, no change [{Show(none)}] [{Show(same)}] [{Show(already)}]");

        // ---- SET-OMITTED: one Ring0 entry ----
        var loose = Loose(p, ringMana) ?? Loose(p, ringShock) ?? Loose(p, ringLight);
        var held = Items.Held(p);
        if (loose is null) Check(false, $"SET-OMITTED: no ring in the backpack to put on: {Gear(p)}");
        else
        {
            var ringRef = loose.Ref;
            var r = await Items.EquipSet(p, Set(scope, (EquipmentSlot.Ring0, new InstanceEquipment(ringRef))));
            await Steps(1);
            var after = Items.CaptureSet(p).Set!;
            var others = captured.Entries.Where(e => e.Slot != EquipmentSlot.Ring0).Zip(after.Entries.Where(e => e.Slot != EquipmentSlot.Ring0))
                .All(x => x.First == x.Second);
            Check(r is { Status: EquipmentSetStatus.Done, Committed: true } && Items.Equipped(p, EquipmentSlot.Ring0) == ringRef && others && Items.Held(p) == held &&
                  r.Changes.Count == 1 && r.Changes[0].Slot == EquipmentSlot.Ring0 && ChangedFor(r.Ticket) == 1 &&
                  _changed.Last(e => e.Ticket == r.Ticket).Changes.Count == 1,
                $"SET-OMITTED: one Ring0 entry changes Ring0 only (the other 19 slots and the hand unchanged), one event [{Show(r)}]");
            Check(!deciding || DecisionsFor(r.Ticket) == 1, $"SET-OMITTED: one PlayerEquippingEvent for the set ({DecisionsFor(r.Ticket)})");

            // ---- SET-EMPTY: an explicit empty takes off only that slot ----
            gear = Gear(p);
            var off = await Items.EquipSet(p, Set(scope, (EquipmentSlot.Ring0, new EmptyEquipment())));
            await Steps(1);
            Check(off is { Status: EquipmentSetStatus.Done, Committed: true } && Items.Equipped(p, EquipmentSlot.Ring0).IsNone && off.Changes.Count == 1 &&
                  off.Changes[0].Before.Instance == ringRef && off.Changes[0].After.Item.IsNone && Loose(p, ringRef.Item) is { } returned && returned.Ref == ringRef,
                $"SET-EMPTY: an explicit empty takes the ring off into the backpack, nothing else changes [{Show(off)}]");
        }

        // a three-slot set from the backpack: Head, Ring1, Arrow (used by the rows below)
        EquipmentSet? Three()
        {
            var now = Items.CaptureSet(p).Set;
            if (now is null) return null;
            var h = Loose(p, helmetB) ?? Loose(p, helmetA);
            var ring = Loose(p, ringShock) ?? Loose(p, ringLight) ?? Loose(p, ringMana);
            var arrowsWorn = now.Entries.First(e => e.Slot == EquipmentSlot.Arrow).Selection is StackEquipment s ? s.Item : ItemId.None;
            var arrows = Loose(p, arrowsWorn == flint ? scrap : flint);
            if (h is null || ring is null || arrows is null) return null;
            return Set(now.Scope, (EquipmentSlot.ArmourHead, new InstanceEquipment(h.Ref)), (EquipmentSlot.Ring1, new InstanceEquipment(ring.Ref)),
                (EquipmentSlot.Arrow, new StackEquipment(arrows.Item, arrows.Slot, arrows.Count)));
        }

        // one item into the worn head slot (a single swap; more than one swap is refused, see SET-SWAPS)
        EquipmentSet? OneSwap()
        {
            var now = Items.CaptureSet(p).Set;
            var worn = Items.Equipped(p, EquipmentSlot.ArmourHead);
            var other = worn.IsNone ? null : Loose(p, worn.Item == helmetA ? helmetB : helmetA);
            return now is null || other is null ? null : Set(now.Scope, (EquipmentSlot.ArmourHead, new InstanceEquipment(other.Ref)));
        }

        // ---- SET-SINGLE-DECISION: one typed decision for the set; a cancel leaves everything ----
        var three = Three();
        if (three is null) Check(false, $"three-slot set: the fixtures are missing: {Gear(p)}");
        else if (!deciding) Check(false, $"SET-SINGLE-DECISION: events.equip is off ({Capabilities.Why("events.equip")}): no handler can decide a set");
        else
        {
            gear = Gear(p);
            _cancelNext = true;
            var canceled = await Items.EquipSet(p, three);
            _cancelNext = false;
            var ev = _deciding.LastOrDefault(e => e.Change is SetEquipmentChange s && s.ItemsTicket == canceled.Ticket);
            Check(canceled is { Status: EquipmentSetStatus.Canceled, Ok: false, Committed: false, Changes.Count: 0 } && DecisionsFor(canceled.Ticket) == 1 &&
                  ev is { Change: SetEquipmentChange { Set.Entries.Count: 3, DryRun: false } } && canceled.Decision == ev.Id && ChangedFor(canceled.Ticket) == 0 &&
                  Gear(p) == gear,
                $"SET-SINGLE-DECISION: one SetEquipmentChange for three slots, cancel -> Canceled, nothing changed [{Show(canceled)}]");
        }

        // ---- SET-DRY ----
        three = Three();
        if (three is not null)
        {
            gear = Gear(p);
            var dry = await Items.EquipSet(p, three, EquipOptions.DryRun);
            Check(dry is { Status: EquipmentSetStatus.Simulated, Ok: true, DryRun: true, Committed: false, Changes.Count: 0 } && ChangedFor(dry.Ticket) == 0 &&
                  Gear(p) == gear && (!deciding || _deciding.Any(e => e.Change is SetEquipmentChange { DryRun: true } s && s.ItemsTicket == dry.Ticket)),
                $"SET-DRY: Simulated, DryRun, not committed, no changes, no event, the inventory as before [{Show(dry)}]");
        }

        // ---- SET-ROLLBACK: a refusal of the game at the first, a middle and the last step ----
        for (var k = 0; k < 3; k++)
        {
            three = Three();
            if (three is null) break;
            gear = Gear(p);
            var before = Items.CaptureSet(p).Set!.Entries;
            var r = await Items.LabEquipSet(p, three, LabSetFault.RefuseStep, k);
            await Steps(1);
            Check(r is { Status: EquipmentSetStatus.Refused, Committed: false, MayHaveMutated: false, Changes.Count: 0 } && r.FailedOperation == k &&
                  r.GameError != 0 && ChangedFor(r.Ticket) == 0 && Gear(p) == gear && Items.CaptureSet(p).Set!.Entries.SequenceEqual(before),
                $"SET-ROLLBACK: the game refused step {k} of 3: Refused, rolled back, kiln.items' snapshot and this read as before [{Show(r)}]");
        }

        // ---- SET-COMMIT-ROLLBACK ----
        three = Three();
        if (three is not null)
        {
            gear = Gear(p);
            var guard = await Items.LabEquipSet(p, three, LabSetFault.GuardBit);
            Check(guard is { Status: EquipmentSetStatus.Refused, Committed: false } && guard.Reason?.Contains("roll the transaction back by itself") == true &&
                  ChangedFor(guard.Ticket) == 0 && Gear(p) == gear,
                $"SET-COMMIT-ROLLBACK: the commit's rollback bit set: refused before the commit, rolled back once [{Show(guard)}]");
            three = Three()!;
            var anyway = await Items.LabEquipSet(p, three, LabSetFault.CommitAnyway);
            await Steps(1);
            Check(anyway is { Status: EquipmentSetStatus.CommitRolledBack, Ok: false, Committed: false, Changes.Count: 0 } && ChangedFor(anyway.Ticket) == 0 &&
                  Gear(p) == gear,
                $"SET-COMMIT-ROLLBACK: committed with the bit set: the game's commit rolled back by itself, CommitRolledBack, no second rollback, no event [{Show(anyway)}]");
        }

        // ---- the three-slot set for real ----
        three = Three();
        if (three is not null)
        {
            var r = await Items.EquipSet(p, three);
            await Steps(1);
            var ok = three.Entries.All(e => e.Selection switch
            {
                InstanceEquipment i => Items.Equipped(p, e.Slot) == i.Item,
                StackEquipment s => Items.CaptureSet(p).Set!.Entries.First(x => x.Slot == e.Slot).Selection is StackEquipment w && w.Item == s.Item && w.ExpectedCount == s.ExpectedCount,
                _ => true,
            });
            Check(r is { Status: EquipmentSetStatus.Done, Committed: true } && r.Changes.Count == 3 && ok && ChangedFor(r.Ticket) == 1,
                $"SET-COMMIT: three slots in one transaction, each as asked, one event with three changes [{Show(r)}]");
        }

        // ---- SET-SWAP: one item into a slot that holds another (the game's equip swaps them) ----
        {
            var worn = Items.Equipped(p, EquipmentSlot.ArmourHead);
            var other = Loose(p, worn.Item == helmetA ? helmetB : helmetA);
            if (worn.IsNone || other is null) Check(false, $"SET-SWAP: a worn helmet and another one in the backpack are needed: {Gear(p)}");
            else
            {
                Log.Info($"equipment-sets-test: before the swap: {Gear(p)}");
                var r = await Items.EquipSet(p, Set(Items.CaptureSet(p).Set!.Scope, (EquipmentSlot.ArmourHead, new InstanceEquipment(other.Ref))));
                await Steps(1);
                Log.Info($"equipment-sets-test: after the swap: {Gear(p)}");
                Check(r is { Status: EquipmentSetStatus.Done, Committed: true } && Items.Equipped(p, EquipmentSlot.ArmourHead) == other.Ref &&
                      Loose(p, worn.Item) is { } off && off.Ref == worn && off.Slot == other.Slot && r.Changes.Count == 1,
                    $"SET-SWAP: a helmet into the worn head slot: Done, the worn one in the new one's slot [{Show(r)}]");
            }
        }
        if (!Capabilities.Is("items.equip_sets")) return;

        // ---- SET-SWAPS: several slots that hold items, each to get another: refused before mutation ----
        three = Three();
        if (three is not null)
        {
            gear = Gear(p);
            var plan = Items.PlanSet(p, three);
            var r = await Items.EquipSet(p, three);
            Check(!plan.Ok && plan.Reason?.StartsWith("UnsupportedSwaps") == true && r is { Status: EquipmentSetStatus.Refused, Committed: false } &&
                  r.Reason?.StartsWith("UnsupportedSwaps") == true && Gear(p) == gear && ChangedFor(r.Ticket) == 0,
                $"SET-SWAPS: three worn slots each to get another item: refused before mutation [{plan.Reason}] [{Show(r)}]");
        }

        // ---- SET-SNAPSHOT: a plan goes stale when the inventory changes ----
        three = OneSwap();
        if (three is not null)
        {
            var plan = Items.PlanSet(p, three);
            Check(plan is { Ok: true, Plan: { Steps.Count: 1 } }, $"SET-SNAPSHOT: a plan of one step ({plan.Reason}; {plan.Plan?.Steps.Count} steps, {plan.Plan?.RequiredFreeSlots} free slot(s))");
            if (plan.Plan is { } pl)
            {
                await Items.Give(p, flint, 1);  // the inventory changes after the snapshot
                await Steps(2);
                gear = Gear(p);
                var stale = await Items.EquipSet(pl);
                Check(stale is { Status: EquipmentSetStatus.StaleSnapshot, Committed: false, Changes.Count: 0 } && Gear(p) == gear && ChangedFor(stale.Ticket) == 0,
                    $"SET-SNAPSHOT: the plan after a give: StaleSnapshot before the transaction, nothing changed [{Show(stale)}]");
                var again = await Items.EquipSet(pl);
                Check(again is { Status: EquipmentSetStatus.StaleSnapshot, Ticket: 0 }, $"SET-SNAPSHOT: a plan applies once [{Show(again)}]");
                var disposed = Items.PlanSet(p, OneSwap()!).Plan!;
                disposed.Dispose();
                var gone = await Items.EquipSet(disposed);
                Check(gone is { Status: EquipmentSetStatus.StaleSnapshot }, $"SET-SNAPSHOT: a disposed plan is stale [{Show(gone)}]");
            }
        }

        // ---- SET-CALLBACK: once, copies valid after it, a cancel before it ran ----
        three = OneSwap();
        if (three is not null)
        {
            gear = Gear(p);
            var canceled = await Items.LabEquipSet(p, three, LabSetFault.CancelAtOnce);
            Check(canceled is { Status: EquipmentSetStatus.Dropped, Committed: false } && Gear(p) == gear && ChangedFor(canceled.Ticket) == 0,
                $"SET-CALLBACK: canceled before it ran (as when its mod unloads): Dropped once, nothing changed [{Show(canceled)}]");
            var r = await Items.EquipSet(p, OneSwap()!);
            await Steps(3);
            Check(r is { Status: EquipmentSetStatus.Done } && r.Changes.All(c => c.After.Instance.IsNone || Items.Equipped(p, c.Slot) == c.After.Instance) &&
                  ChangedFor(r.Ticket) == 1,
                $"SET-CALLBACK: the result once, its copied changes still name what each slot holds three steps later [{Show(r)}]");
        }

        if (!Capabilities.Is("items.equip_sets")) return;

        // ---- SET-SESSION-ONLY: the JSON keeps the scope; another process, world or life is refused ----
        var now2 = Items.CaptureSet(p).Set!;
        var json = EquipmentSetJson.Serialize(now2);
        var back = EquipmentSetJson.Deserialize(json);
        var round = await Items.EquipSet(p, back);
        var process = await Items.EquipSet(p, back with { Scope = back.Scope with { ProcessSession = Guid.NewGuid() } });
        var world = await Items.EquipSet(p, back with { Scope = back.Scope with { WorldEpoch = back.Scope.WorldEpoch + 1 } });
        var life = await Items.EquipSet(p, back with { Scope = back.Scope with { Player = back.Scope.Player with { LifeSerial = back.Scope.Player.LifeSerial + 1000 } } });
        Check(round.Status == EquipmentSetStatus.NoChange && process.Status == EquipmentSetStatus.StaleSnapshot && world.Status == EquipmentSetStatus.StaleSnapshot &&
              life.Status == EquipmentSetStatus.StaleSnapshot,
            $"SET-SESSION-ONLY: a JSON round trip applies in this session (NoChange); another process, server world or life: StaleSnapshot, never re-resolved " +
            $"[{Show(round)}] [{Show(process)}] [{Show(world)}] [{Show(life)}] ({json.Length} bytes)");

        // ---- SET-BACKPACK / CYCLE ----
        var cap3 = Items.CaptureSet(p).Set!;
        var worn0 = cap3.Entries.First(e => e.Slot == EquipmentSlot.Ring0).Selection;
        var worn1 = cap3.Entries.First(e => e.Slot == EquipmentSlot.Ring1).Selection;
        if (worn0 is EmptyEquipment)
        {
            var any = Loose(p, ringLight) ?? Loose(p, ringMana) ?? Loose(p, ringShock);
            if (any != null) await Items.EquipSet(p, Set(cap3.Scope, (EquipmentSlot.Ring0, new InstanceEquipment(any.Ref))));
            await Steps(1);
            cap3 = Items.CaptureSet(p).Set!;
            worn0 = cap3.Entries.First(e => e.Slot == EquipmentSlot.Ring0).Selection;
            worn1 = cap3.Entries.First(e => e.Slot == EquipmentSlot.Ring1).Selection;
        }
        if (worn0 is InstanceEquipment r0 && worn1 is InstanceEquipment r1)
        {
            gear = Gear(p);
            var swap = Set(cap3.Scope, (EquipmentSlot.Ring0, new InstanceEquipment(r1.Item)), (EquipmentSlot.Ring1, new InstanceEquipment(r0.Item)));
            var plan = Items.PlanSet(p, swap);
            plan.Plan?.Dispose();
            if (plan.Ok)
            {
                var r = await Items.EquipSet(p, swap);
                await Steps(1);
                Check(r is { Status: EquipmentSetStatus.Done } && Items.Equipped(p, EquipmentSlot.Ring0) == r1.Item && Items.Equipped(p, EquipmentSlot.Ring1) == r0.Item &&
                      r.Changes.Count == 2,
                    $"SET-CYCLE: the two worn rings swapped through a free slot: Done, both as asked [{Show(r)}]");
                var swapBack = Set(Items.CaptureSet(p).Set!.Scope, (EquipmentSlot.Ring0, new InstanceEquipment(r0.Item)), (EquipmentSlot.Ring1, new InstanceEquipment(r1.Item)));
                var fail = await Items.LabEquipSet(p, swapBack, LabSetFault.RefuseStep, 2);
                Check(fail is { Status: EquipmentSetStatus.Refused, FailedOperation: 2 } && Items.Equipped(p, EquipmentSlot.Ring0) == r1.Item,
                    $"SET-CYCLE: the cycle refused at its last step (a slot emptied and filled again in one transaction): Refused, the snapshot as before [{Show(fail)}]");
            }
            else
            {
                var r = await Items.EquipSet(p, swap);
                Check(plan.Reason?.StartsWith("UnsupportedCycle") == true && r is { Status: EquipmentSetStatus.Refused, Committed: false } &&
                      r.Reason?.StartsWith("UnsupportedCycle") == true && Gear(p) == gear,
                    $"SET-CYCLE: two worn rings swapped: refused before mutation (UnsupportedCycle: no free slot, or kiln/data/kiln.items/sets-no-cycles) [{plan.Reason}] [{Show(r)}]");
            }
        }
        else Check(false, $"SET-CYCLE: two worn rings are needed: {worn0} / {worn1}");
        var pack = Loose(p, Id("Backpack_T1_NEW"));
        if (pack is null)
        {
            await Items.Give(p, Id("Backpack_T1_NEW"), 1);
            await Steps(3);
            pack = Loose(p, Id("Backpack_T1_NEW"));
        }
        if (pack is not null)
        {
            gear = Gear(p);
            var plan = Items.PlanSet(p, Set(Items.CaptureSet(p).Set!.Scope, (EquipmentSlot.Backpack, new InstanceEquipment(pack.Ref))));
            plan.Plan?.Dispose();
            Check(plan.Ok ? true : plan.Reason?.StartsWith("UnsupportedBackpack") == true && Gear(p) == gear,
                $"SET-BACKPACK: a backpack change is {(plan.Ok ? "planned (kiln/data/kiln.items/sets-backpack)" : "refused before mutation")} [{plan.Reason}]");
        }
        else Check(false, $"SET-BACKPACK: no backpack to try: {Gear(p)}");

        // ---- SET-INVARIANT (last: it switches equipment sets off until the server restarts) ----
        var last = Items.CaptureSet(p).Set!;
        var target = Loose(p, ringMana) ?? Loose(p, ringLight) ?? Loose(p, ringShock);
        if (target is null) Check(false, $"SET-INVARIANT: no ring in the backpack: {Gear(p)}");
        else
        {
            var r = await Items.LabEquipSet(p, Set(last.Scope, (EquipmentSlot.Ring0, new InstanceEquipment(target.Ref))), LabSetFault.FakeMismatch);
            await Steps(30);
            var off = !Capabilities.Is("items.equip_sets");
            var threw = false;
            try
            {
                _ = Items.CaptureSet(p);
            }
            catch (KilnUnavailableException)
            {
                threw = true;
            }
            Check(r is { Status: EquipmentSetStatus.InvariantFailure, Ok: false, MayHaveMutated: true, Committed: true } && r.Changes.Count == 1 &&
                  _faulted.Count(f => f.Result.Ticket == r.Ticket) == 1 && _changed.Any(e => e.Ticket == r.Ticket && e.Outcome == EquipmentSetStatus.InvariantFailure) &&
                  off && threw,
                $"SET-INVARIANT: a mismatch after the commit: InvariantFailure, MayHaveMutated, the real change reported, one faulted event, sets off " +
                $"({Capabilities.Why("items.equip_sets")}) [{Show(r)}]");
        }
    }
}
