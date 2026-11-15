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
//  WATCH-FROZEN        (release mode) a batch frozen and not read (the lab hold): a watch disposed and an owner unloaded
//                      while their records wait in it are taken out of it, the survivor's record arrives once, the batch
//                      is acknowledged; a batch emptied that way gives its frame back, so a new watch's next two frames
//                      queue without an overflow; at the end a frozen batch waits for the server's stop
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
            // the commit's step: kiln.items' writes; the game may touch SlotSelection again in a later step (Unknown)
            var first = mine.Length == 0 ? 0 : mine.Min(c => c.Step);
            var commit = mine.Where(c => c.Step == first).ToArray();
            Check(done.Status == EquipmentSetStatus.Done && commit.Length >= 1 && commit.All(c => c.Origin is ChangeOrigin.Kiln && c.SourceMod == "kiln.items"),
                $"WATCH-SET-ROLLBACK path=commit status={done.Status}: {commit.Length} change(s) in the commit's step, all kiln.items' writes " +
                $"({mine.Length - commit.Length} later, the game's: {string.Join(", ", mine.Except(commit).Select(c => $"{c.Component} {c.Origin}"))}): {string.Join(" ", mine.Select(Rec))}");
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
            // a chest (the stand-ins' bags are full of pouches; their free slots are equipment slots)
            root = w.Request.Entities.FirstOrDefault(e => chests.Contains(e) && Items.Read(e)?.Slots.Any(s => s.IsEmpty) == true);
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

    // ---- WATCH-FROZEN (release mode) ----
    private async Task Frozen(string run, bool load)
    {
        if (load) await Mods.LoadAsync(Helper);
        var h = await HelperReady(_lastNonce);
        if (h is not { } helper)
        {
            Check(false, $"[{run}] WATCH-FROZEN: the helper never said it was ready");
            return;
        }
        await Wait(5);
        var made = await SpawnPlayers(3, new WorldPosition(3885, 835, run == "plain" ? 1480 : 1500));
        await Wait(30);
        if (made.Count < 3)
        {
            Check(false, $"[{run}] WATCH-FROZEN: {made.Count} stand-in(s)");
            return;
        }
        var (a, b, d) = (made[0], made[1], made[2]);
        var wa = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [a], ObservedComponents.Health)).Watch;
        var wb = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [b], ObservedComponents.Health)).Watch;
        var wd = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [d], ObservedComponents.Health)).Watch;
        ComponentWatch? we = null;
        try
        {
            if (wa is null || wb is null || wd is null)
            {
                Check(false, $"[{run}] WATCH-FROZEN: the watches were not made");
                return;
            }
            await Wait(4);

            // 1. a disposed watch and an unloaded owner leave a frozen batch; the survivor's record arrives once
            _changes.Clear();
            var overflows = Counter("overflows");
            var helperEvents = EventsOf(helper.Nonce);
            if (!ComponentChanges.LabHoldDelivery(10))
            {
                Log.Info($"{Id}: WATCH-FROZEN: not run, no kiln/control/watches.lab");
                return;
            }
            Combat.SetHealth(a, Hp(a) - 1);
            Combat.SetHealth(b, Hp(b) - 1);
            Combat.SetHealth(helper.Root, Hp(helper.Root) - 1);
            await Wait(2);
            var batch = Counter("lab_frozen_batch");
            var frozen = Counter("lab_frozen_records");
            var frozenLine = Counters();
            wb.Dispose();
            var unloaded = await Mods.UnloadAsync(Helper);
            await Wait(1);
            var left = Counter("lab_frozen_records");
            var leftQueued = Counter("queued_records");
            var leftLine = Counters();
            ComponentChanges.LabHoldDelivery(0);
            await Wait(3);
            var ofA = _changes.Where(c => c.Entity == a).ToArray();
            var ofB = _changes.Where(c => c.Entity == b).ToArray();
            var helperMore = EventsOf(helper.Nonce) - helperEvents;
            Check(frozen == 3 && left == 1 && leftQueued == 1 && ofA.Length == 1 && ofA[0].WatchIds.SequenceEqual([wa.Id]) && ofB.Length == 0 &&
                  helperMore == 0 && unloaded.Ok && Counter("queued_frames") == 0 && Counter("queued_records") == 0 && Counter("overflows") == overflows,
                $"[{run}] WATCH-FROZEN purge: batch {batch} frozen with {frozen} record(s) (A {a.Value}, B {b.Value}, {Helper}'s root {helper.Root.Value}); " +
                $"B's watch disposed and {Helper} unloaded ({unloaded.Ok}, {unloaded.Message}) while it waited: {left} record(s) left in it " +
                $"(queued_records {leftQueued}); after the hold: A's change delivered {ofA.Length} time(s), B's {ofB.Length}, {Helper}'s handler " +
                $"{helperMore} more line(s); acknowledged: queued_frames {Counter("queued_frames")} queued_records {Counter("queued_records")} " +
                $"overflows_delta={Counter("overflows") - overflows}; {string.Join(" ", ofA.Select(Rec))}; frozen: {frozenLine}; after the ends: {leftLine}");

            // 2. a frozen batch emptied by its watch's end gives its frame back: a new watch's next two frames queue, no overflow
            _changes.Clear();
            overflows = Counter("overflows");
            ComponentChanges.LabHoldDelivery(10);
            Combat.SetHealth(d, Hp(d) - 1);
            await Wait(2);
            var batch2 = Counter("lab_frozen_batch");
            var frozen2 = Counter("lab_frozen_records");
            var framesFrozen = Counter("queued_frames");
            wd.Dispose();
            await Wait(1);
            var framesAfter = Counter("queued_frames");
            var recordsAfter = Counter("lab_frozen_records");
            we = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [b], ObservedComponents.Health)).Watch;
            await Wait(1);  // its baseline
            Combat.SetHealth(b, Hp(b) - 1);
            await Wait(1);
            Combat.SetHealth(b, Hp(b) - 1);
            await Wait(1);
            var framesQueued = Counter("queued_frames");
            var overflowed = Counter("overflows") - overflows;
            var state = we?.State;
            var queuedLine = Counters();
            ComponentChanges.LabHoldDelivery(0);
            await Wait(5);
            var ofNew = _changes.Where(c => c.Entity == b && we is not null && c.WatchIds.Contains(we.Id)).ToArray();
            Check(frozen2 == 1 && framesFrozen == 1 && recordsAfter == 0 && framesAfter == 0 && framesQueued == 2 && overflowed == 0 &&
                  state == ComponentWatchState.Ready && ofNew.Length == 2 && _changes.All(c => c.Entity != d) && Counter("queued_frames") == 0,
                $"[{run}] WATCH-FROZEN frame back: batch {batch2} frozen with {frozen2} record(s) of D {d.Value} (queued_frames {framesFrozen}); D's watch " +
                $"disposed: {recordsAfter} record(s) left, queued_frames {framesAfter} (the frame given back, the id still to be acknowledged); a new watch " +
                $"on B, two changes in two held steps: queued_frames {framesQueued}, overflows_delta={overflowed}, the watch {state}; after the hold: " +
                $"{ofNew.Length} change(s) delivered to it, D's {_changes.Count(c => c.Entity == d)}; {string.Join(" ", ofNew.Select(Rec))}; while held: {queuedLine}");
        }
        finally
        {
            wa?.Dispose();
            wb?.Dispose();
            wd?.Dispose();
            we?.Dispose();
            await Wait(2);
        }
        if (run == "plain")
        {
            _lastNonce = helper.Nonce;
            await Mods.LoadAsync(Helper);  // the parallel run starts from a fresh load
        }
    }

    // At the end of the release run: a batch frozen with a record waits for the server's stop (the hold is armed again
    // until this mod unloads). The stop's log says what happened to it.
    private async Task FrozenAtStop()
    {
        var made = await SpawnPlayers(1, new WorldPosition(3885, 835, 1520));
        await Wait(30);
        if (made.Count == 0) return;
        var w = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [made[0]], ObservedComponents.Health)).Watch;
        if (w is null) return;
        await Wait(4);
        if (!ComponentChanges.LabHoldDelivery(10)) return;
        Combat.SetHealth(made[0], Hp(made[0]) - 1);
        await Wait(2);
        Log.Info($"{Id}: STOP-FROZEN: batch {Counter("lab_frozen_batch")} frozen with {Counter("lab_frozen_records")} record(s) of watch {w.Id} " +
                 $"waits for the server's stop (not disposed; the hold is armed again until this mod unloads): {Counters()}");
        File.WriteAllText(Path.Combine(Context.DataDirectory, "stop-frozen.txt"), $"{Counter("lab_frozen_batch")}\n");
        while (!_unloading)
        {
            await Wait(8);
            if (!_unloading) ComponentChanges.LabHoldDelivery(10);
        }
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
