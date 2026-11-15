// SPDX-License-Identifier: MIT
// component-watch-test, the rows the 07 S7 review asked for that a dedicated server can show (each logs its numbers
// and the raw records it judged, not only PASS):
//  WATCH-OVERLAP       a change in Pre, a second watch on the same pair, a change in Sim: two records of one pair in one
//                      step (the old watch's interval ends where the new one's starts)
//  WATCH-REUSE         a root destroyed and a stand-in spawned in the same Pre: the old watch SuspendedMetadata, no event
//                      under the old key (the reason says which window saw it); a linked item that leaves every tree and
//                      comes back gets a new life serial
//  WATCH-SET-ROLLBACK  the equipment-set faults (kiln/data/kiln.items/sets-lab) under an active watch: a refusal after a
//                      staged move, the commit's rollback bit, the commit that rolls back by itself, a dry run: raw
//                      Inventory and SlotSelection bytes before equal after, no known token, no change
//  WATCH-QUEUE         the consumer held (kiln/control/watches.lab): two frames retained, the third overflows, every ready
//                      watch suspended, LastCompleteStep unchanged; drained, Resume, no catch-up
//  WATCH-NOTE-COST     a give/take job's note with no watch, with a watch on an unrelated entity, with one on its holder
//                      (kiln/data/kiln.items/notes-lab: kiln.items logs the job's and the note's p50, p99 and max)
//  WATCH-GROWTH        at 4096 present pairs a watched chest gets an item entity: that watch is suspended (budget) before
//                      the step's compare and reports nothing of that step
//  WATCH-COST-ABSENT   the window calls with mostly absent pairs (entities with SpawnTime, components they lack)
using Kiln;

namespace ComponentWatchTest;

public sealed partial class ComponentWatchTestMod
{
    private static string Hex(ReadOnlyMemory<byte> b) => Convert.ToHexString(b.Span);

    private static string Rec(ComponentValueChangedEvent c) =>
        $"{{entity {c.Entity.Value} life {c.Key.LifeSerial:x} {c.Component} step {c.Step} watches [{string.Join(",", c.WatchIds)}] " +
        $"origin {c.Origin}/{c.SourceMod ?? "none"} tokens {c.OperationTokens.Count} before {Hex(c.Before)[..Math.Min(32, c.Before.Length * 2)]} " +
        $"after {Hex(c.After)[..Math.Min(32, c.After.Length * 2)]}}}";

    // ---- WATCH-OVERLAP ----
    private async Task Overlap(EntityId p)
    {
        var w1 = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [p], ObservedComponents.Health));
        if (w1.Watch is null)
        {
            Check(false, $"WATCH-OVERLAP: the first watch is refused ({w1.Reason})");
            return;
        }
        ComponentWatch? w2 = null;
        try
        {
            await Wait(4);
            _changes.Clear();
            var hp = Hp(p);
            Combat.SetHealth(p, hp - 1);                                // Pre: kiln.combat's write
            w2 = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [p], ObservedComponents.Health)).Watch;  // a second watch
            Volatile.Write(ref _rawDone, 0);
            Volatile.Write(ref _rawTarget, (int)p.Value);               // Sim: this mod's program, no token
            await Wait(4);
            var recs = Of(p, "Health");
            var sameStep = recs.Length == 2 && recs[0].Step == recs[1].Step;
            Check(w2 is not null && sameStep && recs[0].WatchIds.SequenceEqual([w1.Watch.Id]) && recs[1].WatchIds.Contains(w1.Watch.Id) &&
                  recs[1].WatchIds.Contains(w2!.Id) && recs[0].After.Span.SequenceEqual(recs[1].Before.Span),
                $"WATCH-OVERLAP records={recs.Length} same_step={(sameStep ? 1 : 0)}: a change in Pre, a second watch, a change in Sim " +
                $"(raw write {Volatile.Read(ref _rawDone)}): {string.Join(" ", recs.Select(Rec))}");
        }
        finally
        {
            w1.Watch.Dispose();
            w2?.Dispose();
            await Wait(2);
        }
    }

    // ---- WATCH-REUSE ----
    private async Task Reuse(WorldPosition at)
    {
        var made = await SpawnPlayers(1, new WorldPosition(at.X + 30, at.Y, at.Z));
        if (made.Count == 0)
        {
            Check(false, "WATCH-REUSE: no stand-in");
            return;
        }
        var r = made[0];
        await Wait(30);
        var w = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [r], ObservedComponents.Health));
        if (w.Watch is null)
        {
            Check(false, $"WATCH-REUSE: the watch is refused ({w.Reason})");
            return;
        }
        try
        {
            await Wait(3);
            _changes.Clear();
            _states.Clear();
            Combat.SetHealth(r, Hp(r) - 2);
            await Wait(3);
            var oldKey = Of(r, "Health").FirstOrDefault()?.Key;
            // destroy and spawn in one Pre: an id the game frees may come back for the next entity
            _changes.Clear();
            var step = Counter("windows");
            Spawn.Destroy(r);
            var existsAfter = Spawn.Exists(r);
            var spawning = Spawn.Entity("1_Player", new WorldPosition(at.X + 30, at.Y, at.Z), options: new SpawnOptions { Tag = Id, Flags = SpawnFlags.Transient });
            var n = await spawning;
            if (!n.IsNone) _spawned.Add(n);
            await Wait(6);
            if (!n.IsNone && Hp(n) > 3) Combat.SetHealth(n, Hp(n) - 3);
            await Wait(3);
            var s = _states.FirstOrDefault(x => x.WatchId == w.Watch.Id);
            var underOld = _changes.Count(c => oldKey is { } k && c.Key == k);
            var reused = n.Value == r.Value;
            var newKey = reused ? _changes.FirstOrDefault(c => c.Entity == n)?.Key : null;
            Check(oldKey is not null && w.Watch.State == ComponentWatchState.SuspendedMetadata && underOld == 0,
                $"WATCH-REUSE root: old_key={oldKey?.LifeSerial:x} new_entity={n.Value} reused_id={(reused ? 1 : 0)} " +
                $"new_key={newKey?.LifeSerial:x} old_watch={w.Watch.State} ({s?.Reason ?? w.Watch.Reason}) replacement_events={underOld}; " +
                $"the destroy was {(existsAfter ? "deferred (the entity still existed in that Pre)" : "immediate")} (windows {step} -> {Counter("windows")})");
        }
        finally
        {
            w.Watch.Dispose();
            await Wait(2);
        }
    }

    // a linked item leaving every tree and coming back: a new life serial. Two chests (a stand-in's pouch may sit in an
    // equipped quick slot, which a move refuses): the watched one, and another nothing watches.
    private async Task LinkedReentry(WorldPosition at)
    {
        var two = await Chests(2, new WorldPosition(at.X - 20, at.Y, at.Z - 10), 0);
        if (two.Count < 2)
        {
            Check(false, "WATCH-REUSE linked: no two chests");
            return;
        }
        var (q, outside) = (two[0], two[1]);
        var given = await Items.Give(q, Pouch, 1);
        await Wait(3);
        var slot = Items.Read(q)?.Slots.FirstOrDefault(s => s.Item == given.Item && !s.ItemEntity.IsNone);
        if (slot is null)
        {
            Check(false, $"WATCH-REUSE linked: no pouch with an item entity in chest {q.Value} ({given.Status})");
            return;
        }
        var w = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [q], ObservedComponents.Fuel | ObservedComponents.Inventory,
            ComponentWatchExpansion.PlayerInventoryAndItems));
        if (w.Watch is null)
        {
            Check(false, $"WATCH-REUSE linked: the watch is refused ({w.Reason})");
            return;
        }
        try
        {
            await Wait(3);
            var x = slot.ItemEntity;
            _changes.Clear();
            await Items.Fuel.Set(x, 10);
            await Wait(4);
            var first = Of(x, "Fuel").LastOrDefault();
            var gone = await Items.Move(q, slot.Slot, outside);
            await Wait(6);  // observed outside every tree
            var there = Items.Read(outside)?.Slots.FirstOrDefault(s => s.Item == given.Item && !s.IsEmpty);
            var back = there is null ? null : await Items.Move(outside, there.Slot, q);
            await Wait(4);
            var y = Items.Read(q)?.Slots.FirstOrDefault(s => s.Item == given.Item && !s.ItemEntity.IsNone)?.ItemEntity ?? default;
            _changes.Clear();
            if (!y.IsNone) await Items.Fuel.Set(y, 20);
            await Wait(4);
            var again = y.IsNone ? null : Of(y, "Fuel").LastOrDefault();
            Check(gone.Succeeded && back is { Succeeded: true } && first is not null && again is not null && first.Key.LifeSerial != again.Key.LifeSerial,
                $"WATCH-REUSE linked: item entity {x.Value} in chest {q.Value} -> moved out to {outside.Value} ({gone.Status} {gone.Message}) and back ({back?.Status} {back?.Message}) as {y.Value} " +
                $"(same id {(x == y ? 1 : 0)}): serial {first?.Key.LifeSerial:x} -> {again?.Key.LifeSerial:x}; {(first is null ? "" : Rec(first))} {(again is null ? "" : Rec(again))}");
        }
        finally
        {
            w.Watch.Dispose();
            await Wait(2);
        }
    }

    // ---- WATCH-SET-ROLLBACK ----
    private static ItemId ItemOf(string debugName) => Items.Lookup(debugName)?.Id ?? default;

    private static InventorySlot? Loose(EntityId p, ItemId item) => Items.Read(p)?.Slots.FirstOrDefault(s => s.Item == item && !s.IsEquipped && !s.IsEmpty);

    private static string RawTree(EntityId p)
    {
        var parts = new List<string> { $"SlotSelection {Hex(World.ReadBytes(p, "SlotSelection") ?? [])}" };
        if (Items.Read(p) is { } c)
            foreach (var i in c.Inventories)
                parts.Add($"{i.Inventory.Value}:{Hex(World.ReadBytes(i.Inventory, "Inventory") ?? [])}");
        return string.Join(" ", parts);
    }

    private async Task SetRollback(WorldPosition at)
    {
        if (!Capabilities.Is("items.equip_sets"))
        {
            Log.Info($"{Id}: WATCH-SET-ROLLBACK: not run, items.equip_sets is off here ({Capabilities.Why("items.equip_sets")})");
            return;
        }
        var made = await SpawnPlayers(1, new WorldPosition(at.X + 34, at.Y, at.Z));
        if (made.Count == 0) return;
        var p = made[0];
        await Wait(40);
        var helmetA = ItemOf("Armor_T1_Warrior_DPS_Helmet");
        var helmetB = ItemOf("Armor_T1_Ranger_Archer_DPS_Helmet");
        var light = ItemOf("Ring_T3_Ring_of_Light");
        foreach (var item in new[] { helmetA, helmetB, light }) await Items.Give(p, item, 1);
        await Wait(6);
        if (Loose(p, helmetA) is { } h) await Items.Equip(p, h.Slot, EquipmentSlot.ArmourHead);
        await Wait(2);
        EquipmentSetSelection Pick(ItemId item) =>
            Loose(p, item) is { } s ? (s.Ref.IsNone ? new StackEquipment(s.Item, s.Slot, s.Count) : new InstanceEquipment(s.Ref)) : new EmptyEquipment();
        // a swap, then an equip into an empty slot (two operations; the planner takes one swap per set)
        var set = new EquipmentSet(Items.CaptureSet(p).Set!.Scope,
            [new EquipmentSetEntry(EquipmentSlot.ArmourHead, Pick(helmetB)), new EquipmentSetEntry(EquipmentSlot.Ring1, Pick(light))]);
        var plan = Items.PlanSet(p, set);
        var steps = plan.Plan?.Steps.Count ?? 0;
        plan.Plan?.Dispose();
        var w = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [p], ObservedComponents.Inventory | ObservedComponents.SlotSelection,
            ComponentWatchExpansion.PlayerInventoryAndItems));
        if (!plan.Ok || steps < 2 || w.Watch is null)
        {
            Check(false, $"WATCH-SET-ROLLBACK: no two-step plan ({plan.Reason}, {steps} step(s)) or no watch ({w.Reason})");
            w.Watch?.Dispose();
            return;
        }
        try
        {
            await Wait(3);
            async Task Path(string path, Func<Task<EquipmentSetResult>> run, EquipmentSetStatus expect)
            {
                var before = RawTree(p);
                var tokens = Counter("tokens");
                _changes.Clear();
                var r = await run();
                await Wait(4);
                var after = RawTree(p);
                var mine = _changes.Where(c => c.WatchIds.Contains(w.Watch!.Id)).ToArray();
                var known = Counter("tokens") - tokens;
                Check(r.Status == expect && before == after && known == 0 && mine.Length == 0,
                    $"WATCH-SET-ROLLBACK path={path} status={r.Status} before_equals_after={(before == after ? 1 : 0)} known_tokens_delta={known} " +
                    $"changes={mine.Length} (watch {w.Watch!.State}, {w.Pairs} pair(s)){(before == after ? "" : $" BEFORE {before} AFTER {after}")} {string.Join(" ", mine.Select(Rec))}");
            }
            await Path("refused-after-staged-move", () => Items.LabEquipSet(p, set, LabSetFault.RefuseStep, 1), EquipmentSetStatus.Refused);
            await Path("rollback-bit-before-commit", () => Items.LabEquipSet(p, set, LabSetFault.GuardBit), EquipmentSetStatus.Refused);
            await Path("commit-self-rollback", () => Items.LabEquipSet(p, set, LabSetFault.CommitAnyway), EquipmentSetStatus.CommitRolledBack);
            await Path("dry-run", () => Items.EquipSet(p, set, EquipOptions.DryRun), EquipmentSetStatus.Simulated);
            // and a real one: its writes are kiln.items'
            _changes.Clear();
            var done = await Items.EquipSet(p, set);
            await Wait(4);
            var mine = _changes.Where(c => c.WatchIds.Contains(w.Watch.Id)).ToArray();
            Check(done.Status == EquipmentSetStatus.Done && mine.Length >= 1 && mine.All(c => c.Origin is ChangeOrigin.Kiln or ChangeOrigin.Mixed) &&
                  mine.Any(c => c.SourceMod == "kiln.items"),
                $"WATCH-SET-ROLLBACK path=commit status={done.Status}: {mine.Length} change(s), kiln.items' writes: {string.Join(" ", mine.Select(Rec))}");
        }
        finally
        {
            w.Watch.Dispose();
            await Wait(2);
        }
    }

    // ---- WATCH-QUEUE ----
    private async Task Queue(EntityId a, EntityId b)
    {
        var wa = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [a], ObservedComponents.Health)).Watch;
        var wb = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [b], ObservedComponents.Health)).Watch;
        if (wa is null || wb is null)
        {
            Check(false, "WATCH-QUEUE: the two watches were not made");
            wa?.Dispose();
            wb?.Dispose();
            return;
        }
        try
        {
            await Wait(4);
            _changes.Clear();
            _states.Clear();
            var overflows = Counter("overflows");
            if (!ComponentChanges.LabHoldDelivery(5))
            {
                Log.Info($"{Id}: WATCH-QUEUE: not run, no kiln/control/watches.lab");
                return;
            }
            var hp = Hp(a);
            for (var i = 1; i <= 3; i++)
            {
                Combat.SetHealth(a, hp - i);  // one change in each of three steps
                await Wait(1);
            }
            await Wait(1);
            var held = Counters();
            var queued = Counter("queued_frames");
            var last = wa.LastCompleteStep;
            var states = $"{wa.State}/{wb.State}";
            await Wait(4);  // the hold ends: what was retained is delivered
            var delivered = Of(a, "Health");
            Check(queued == 2 && Counter("overflows") - overflows == 1 && wa.State == ComponentWatchState.SuspendedBudget &&
                  wb.State == ComponentWatchState.SuspendedBudget && wa.LastCompleteStep == last && delivered.Length == 2 &&
                  delivered[^1].Step == last,
                $"WATCH-QUEUE retained_frames={queued} overflows_delta={Counter("overflows") - overflows} ready_watches=0 ({states}, reason {wa.Reason}) " +
                $"last_complete_unchanged={(wa.LastCompleteStep == last ? 1 : 0)} (last complete {last}); delivered after the hold: {string.Join(" ", delivered.Select(Rec))}; held: {held}");
            _changes.Clear();
            var ra = ComponentChanges.Resume(wa);
            var rb = ComponentChanges.Resume(wb);
            await Wait(4);
            var catchUp = Of(a, "Health").Length;
            Combat.SetHealth(a, Hp(a) - 1);
            await Wait(4);
            var next = Of(a, "Health");
            Check(ra.Ok && rb.Ok && wa.State == ComponentWatchState.Ready && catchUp == 0 && next.Length == 1,
                $"WATCH-QUEUE Resume: {wa.State}/{wb.State}, no catch-up ({catchUp} event(s) of the dropped step), the next change reported ({next.Length}): {string.Join(" ", next.Select(Rec))}");
        }
        finally
        {
            wa.Dispose();
            wb.Dispose();
            await Wait(2);
        }
    }

    // ---- WATCH-NOTE-COST ----
    private async Task NoteCost(EntityId q, EntityId unrelated)
    {
        const string Grenade = "Consumable_Grenade_Explosive_PowderBall_T1";
        async Task Jobs(string scenario)
        {
            await Wait(40);  // kiln.items starts a new block after 30 quiet steps
            Log.Info($"{Id}: WATCH-NOTE-COST {scenario}: 200 give/take jobs on {q.Value} start ({Counters()})");
            for (var i = 0; i < 20; i++)
            {
                var gives = Enumerable.Range(0, 5).Select(_ => Items.Give(q, Grenade, 1)).ToArray();
                await Task.WhenAll(gives);
                var item = gives[0].Result.Item;
                var takes = Enumerable.Range(0, 5).Select(_ => Items.Take(q, item, 1)).ToArray();
                await Task.WhenAll(takes);
            }
            Log.Info($"{Id}: WATCH-NOTE-COST {scenario}: done (the kiln.items notes-lab line above has the job's and the note's cost)");
        }
        await Jobs("no watch");
        var u = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [unrelated], ObservedComponents.All, ComponentWatchExpansion.PlayerInventoryAndItems)).Watch;
        await Jobs("a watch on an unrelated entity");
        var own = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [q], ObservedComponents.All, ComponentWatchExpansion.PlayerInventoryAndItems)).Watch;
        await Jobs("a watch on the holder");
        u?.Dispose();
        own?.Dispose();
        await Wait(40);
    }

    // ---- WATCH-GROWTH (in Budget, at 4096 present pairs) ----
    private async Task Growth(List<ComponentWatch> watches, HashSet<EntityId> chests)
    {
        var present = Counter("present");
        // a root of a Ready watch with its inventories expanded and a free slot: one more item entity there is one more
        // present Fuel pair for that watch
        ComponentWatch? offender = null;
        var root = default(EntityId);
        foreach (var w in watches.Where(w => w.State == ComponentWatchState.Ready && w.Request.Expansion == ComponentWatchExpansion.PlayerInventoryAndItems &&
                                             (w.Request.Components & ObservedComponents.Fuel) != 0))
        {
            root = w.Request.Entities.FirstOrDefault(e => Items.Read(e)?.Slots.Any(s => s.IsEmpty && !s.IsEquipped) == true);
            if (root.IsNone) continue;
            offender = w;
            break;
        }
        if (offender is null)
        {
            Check(false, "WATCH-GROWTH: no watched root with its inventories and a free slot");
            return;
        }
        _changes.Clear();
        _states.Clear();
        var given = await Items.Give(root, Pouch, 1);  // a new item entity in a watched page: one more present Fuel pair
        await Wait(4);
        var s = _states.FirstOrDefault(x => x.WatchId == offender.Id);
        var mine = _changes.Where(c => c.WatchIds.Contains(offender.Id)).ToArray();
        Check(present == 4096 && given.Done == 1 && offender.State == ComponentWatchState.SuspendedBudget && mine.Length == 0,
            $"WATCH-GROWTH present={present} (then {Counter("present")}) offender={offender.State} ({s?.Reason ?? offender.Reason}) offender_changes={mine.Length}: " +
            $"Items.Give of a pouch into watched {(chests.Contains(root) ? "chest" : "stand-in")} {root.Value} ({given.Status}); other watches' changes {_changes.Count}, " +
            $"ready {watches.Count(w => w.State == ComponentWatchState.Ready)} of {watches.Count}");
        var resumed = ComponentChanges.Resume(offender);
        Check(!resumed.Ok && (resumed.Reason ?? "").Contains("4096"), $"WATCH-GROWTH: Resume stays suspended while it does not fit: {resumed.Reason}");
    }

    // ---- WATCH-COST-ABSENT ----
    private async Task AbsentCost()
    {
        var mine = new HashSet<EntityId>(_spawned);
        var roots = World.Find("SpawnTime").Where(e => !mine.Contains(e)).Distinct().Take(32 * 128).ToList();
        var watches = new List<ComponentWatch>();
        try
        {
            foreach (var g in roots.Chunk(128))
            {
                var r = ComponentChanges.Watch(new(ComponentWatchScope.Entities, g, ObservedComponents.Mana | ObservedComponents.Fuel | ObservedComponents.SlotSelection | ObservedComponents.Inventory));
                if (r.Watch is { } w) watches.Add(w);
                else Log.Info($"{Id}: WATCH-COST-ABSENT: a group of {g.Length} roots refused: {r.Reason}");
            }
            await Wait(1100);  // the service keeps the last 1024 windows
            Check(watches.Count > 0 && Counter("pairs") > Counter("present") && Counter("samples") > 0,
                $"WATCH-COST-ABSENT: {watches.Count} watches on {roots.Count} entities with SpawnTime (Mana, Fuel, SlotSelection, Inventory): " +
                $"pairs={Counter("pairs")} present={Counter("present")} over 1100 steps: begin p50 {Counter("begin_p50_ns") / 1000.0:F1} us p99 {Counter("begin_p99_ns") / 1000.0:F1} us max {Counter("begin_max_ns") / 1000.0:F1} us, " +
                $"end p50 {Counter("end_p50_ns") / 1000.0:F1} us p99 {Counter("end_p99_ns") / 1000.0:F1} us max {Counter("end_max_ns") / 1000.0:F1} us; {Counters()}");
        }
        finally
        {
            foreach (var w in watches) w.Dispose();
            await Wait(2);
        }
    }
}
