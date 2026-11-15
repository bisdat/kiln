// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Nodes;
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 10-equipment-sets (0.2.0-dev.29): the guided checks of chapter 10. Orders 3200-3299 (lab port 28120).
//
// - 10-items.equip-set: one PlayerEquippingEvent decides a whole set (a veto changes nothing), a dry run changes nothing,
//   and an allowed set of two rings and a helmet appears on the player at once: the player's own client sees no ring
//   alone for a moment and no ghost item in the bag.
// - 10-items.equip-set.observer: the same set with a second player (best one without Kiln) watching: they see no
//   in-between state on the player's character either.
// - 10-items.equip-set.persist and 10-items.equip-set.persist.after: a set puts a shield with its own data (level,
//   rarity, unlocked perks) and a grappling hook with lowered durability into empty slots; part 1 records the items'
//   own bytes in the mod's data folder, the player leaves the world (the game saves the character) and joins again,
//   and part 2 finds both in the same slots with every recorded byte unchanged. The lab cannot show this: a dedicated
//   server's world save does not load a spawned player stand-in (characters are the clients' own). Part 2 puts the
//   player's own shield and hook back (found again by item type and own data: refs do not outlive the session).
//
// items.equip_sets ships on in 0.2.0-dev.29 without these checks (nothing ships held); the lab
// (mods/tests/equipment-sets-test) ran the machinery on a spawned character, never on a player's client. On this game
// build a set may give at most one slot that already holds an item a different one (UnsupportedSwaps, setplan.h), so
// both checks first empty the three targets in a set of removals and then put the test gear into empty slots. The
// player's equipment is put back at the end in the same two supported steps, each from a fresh capture, with the
// original selections captured at the start (never a guessed substitute), and the result is asserted: a set that
// cannot be put back fails the check with the reason.
public sealed partial class ClientPassMod
{
    private const uint SetsRingLight = 0x79442800;  // Ring_T3_Ring_of_Light
    private const uint SetsRingShock = 0x4623bcb0;  // Ring_T6_Ring_of_Shock

    private const string SetsConfirm = "This changes your character's rings and helmet for a while and gives you two rings and a helmet " +
                                       "(your gear is put back at the end). Use a disposable character in a copied world.";

    private static bool IsSetTarget(EquipmentSlot slot) => slot is EquipmentSlot.Ring0 or EquipmentSlot.Ring1 or EquipmentSlot.ArmourHead;

    // the test gear in the player's inventory, the three targets empty, and the set that puts it on
    private sealed record SetFixture(InventorySlot RingA, InventorySlot RingB, InventorySlot Hat, EquipmentSet Set);

    [ClientCheck("10-items.equip-set", "One decision and visible atomic set swap", Area = "Equipment sets", Since = Dev29,
        Needs = ClientCheckNeeds.Hosted, Tags = [Visual], Order = 3200, Confirm = SetsConfirm)]
    public async Task EquipmentSetSwap(CheckContext c)
    {
        if (!await NeedPlayer(c)) return;
        var p = c.Player;
        if (!c.Expect(Capabilities.Is("items.equip_sets"), "equipment sets are on (items.equip_sets)", Capabilities.Why("items.equip_sets")) ||
            !c.Expect(Capabilities.Is("events.equip"), "equipment decisions are on (events.equip): a set can be vetoed", Capabilities.Why("events.equip")))
        {
            await c.Verdict();
            return;
        }
        var original = Items.CaptureSet(p);
        if (!c.Expect(original is { Ok: true, Set: not null }, "your 20 equipment slots were read (the set to put back at the end)", original.Reason))
        {
            await c.Verdict();
            return;
        }
        var decisions = new List<PlayerEquippingEvent>();
        var veto = false;
        var sub = Events.On<PlayerEquippingEvent>(e =>
        {
            if (e.Change is not SetEquipmentChange) return;
            decisions.Add(e);
            if (veto) e.Cancel();
        }, EventFilter.ForEntity(p));
        c.OnEnd(() => sub.Dispose());
        var changed = WatchSets(c, p);
        try
        {
            if (await PrepareSet(c, p, original.Set!) is not { } f)
            {
                await c.Verdict();
                return;
            }

            // 1. a veto: one decision for the whole set, nothing changes
            c.Instruct("Step 1 of 3: Kiln asks to put two rings and a helmet on you as one set, and a test handler refuses it. Nothing " +
                       "should change on your character or in your inventory.");
            var gear = Items.CaptureSet(p).Set!.Entries;
            veto = true;
            var r1 = await Items.EquipSet(p, f.Set);
            veto = false;
            c.Expect(r1 is { Status: EquipmentSetStatus.Canceled, Committed: false, Changes.Count: 0 } &&
                     decisions.Count(e => e.Change is SetEquipmentChange s && s.ItemsTicket == r1.Ticket) == 1 &&
                     Items.CaptureSet(p).Set!.Entries.SequenceEqual(gear),
                "one decision for the three slots, refused: nothing changed", $"{r1.Status} {r1.Reason}; decisions {decisions.Count}");

            // 2. a dry run: nothing changes
            var r2 = await Items.EquipSet(p, f.Set, EquipOptions.DryRun);
            c.Expect(r2 is { Status: EquipmentSetStatus.Simulated, DryRun: true, Committed: false, Changes.Count: 0 } &&
                     changed.All(e => e.Ticket != r2.Ticket) && Items.CaptureSet(p).Set!.Entries.SequenceEqual(gear),
                "a dry run of the set changed nothing", $"{r2.Status} {r2.Reason}");
            await c.Ticks(30);

            // 3. allowed: the three slots at once
            c.Instruct("Step 2 of 3: open your inventory or character screen now and keep watching your rings and helmet. In a few " +
                       "seconds Kiln puts the set on in one transaction: both rings and the helmet should appear at the same moment.");
            await c.Ticks(150);
            await ApplySet(c, p, f, changed);
            c.Instruct("Step 3 of 3: did both rings and the helmet appear at the same time, with no moment where only one ring was worn, " +
                       "an item showed twice, or an item flashed in the bag? Pass only if nothing showed an in-between state. (What a " +
                       "second player sees is 10-items.equip-set.observer.)");
            await c.Verdict();
        }
        finally
        {
            await RestoreSet(c, p, original.Set!);
        }
    }

    [ClientCheck("10-items.equip-set.observer", "A second player sees the atomic set swap", Area = "Equipment sets", Since = Dev29,
        Needs = ClientCheckNeeds.Hosted, Tags = [TwoPlayers, Visual], Order = 3201, Confirm = SetsConfirm)]
    public async Task EquipmentSetObserver(CheckContext c)
    {
        if (!await NeedPlayer(c)) return;
        var p = c.Player;
        if (!c.Expect(Capabilities.Is("items.equip_sets"), "equipment sets are on (items.equip_sets)", Capabilities.Why("items.equip_sets")))
        {
            await c.Verdict();
            return;
        }
        c.Instruct("Have a second player join your world (best one without Kiln) and stand where they can see your character up " +
                   "close. They will watch your rings and helmet.");
        for (var waited = 0; Players.Count < 2 && waited < 120; waited++) await c.Delay(TimeSpan.FromSeconds(1));
        if (!c.Expect(Players.Count >= 2, "a second player is in the hosted world", $"{Players.Count} player(s)"))
        {
            await c.Verdict();
            return;
        }
        var original = Items.CaptureSet(p);
        if (!c.Expect(original is { Ok: true, Set: not null }, "your 20 equipment slots were read (the set to put back at the end)", original.Reason))
        {
            await c.Verdict();
            return;
        }
        var changed = WatchSets(c, p);
        try
        {
            if (await PrepareSet(c, p, original.Set!) is not { } f)
            {
                await c.Verdict();
                return;
            }
            c.Instruct("Step 1 of 2: ask the second player to watch your character now (they can open nothing of yours: they watch " +
                       "your rings and helmet as worn). In a few seconds Kiln puts two rings and a helmet on you in one transaction.");
            await c.Ticks(150);
            await ApplySet(c, p, f, changed);
            c.Instruct("Step 2 of 2: ask the second player what they saw. Did both rings and the helmet appear on your character at the " +
                       "same time for them, with no moment where only one ring was worn and no item that showed twice? Pass only if " +
                       "neither of you saw an in-between state; a ghost item in your bag or a ring that shows on your screen but not on " +
                       "theirs is a fail.");
            await c.Verdict();
        }
        finally
        {
            await RestoreSet(c, p, original.Set!);
        }
    }

    // Every EquipmentSetChangedEvent of the player while the check runs.
    private static List<EquipmentSetChangedEvent> WatchSets(CheckContext c, EntityId p)
    {
        var changed = new List<EquipmentSetChangedEvent>();
        var sub = Events.On<EquipmentSetChangedEvent>(changed.Add, EventFilter.ForEntity(p));
        c.OnEnd(() => sub.Dispose());
        return changed;
    }

    // Steps a restore still waits for after the check was canceled (an early verdict): not c.Ticks, which throws then.
    private static async Task SetSettle(CheckContext c, int steps)
    {
        for (var i = 0; i < steps; i++) await c.Mod.Scheduler.NextTick();
    }

    // A set that empties the target slots (Ring0, Ring1 and ArmourHead unless told otherwise): removals only, which a
    // set may hold together (each takes a free backpack slot). Built from a fresh capture.
    private static EquipmentSet? EmptyTargets(EntityId p, Func<EquipmentSlot, bool>? target = null) =>
        Items.CaptureSet(p).Set is { } now
            ? new EquipmentSet(now.Scope, now.Entries.Where(e => (target ?? IsSetTarget)(e.Slot)).Select(e => new EquipmentSetEntry(e.Slot, new EmptyEquipment())).ToList())
            : null;

    // The fixture: room in the backpack, the test gear given, the three targets emptied (the game may wear a given ring
    // or helmet in an empty slot at once: the removals take that off too), and a set into the empty targets that plans Ok.
    private static async Task<SetFixture?> PrepareSet(CheckContext c, EntityId p, EquipmentSet original)
    {
        var worn = original.Entries.Count(e => IsSetTarget(e.Slot) && e.Selection is not EmptyEquipment);
        var free = Items.Read(p)?.FreeSlots() ?? 0;
        if (!c.Expect(free >= 3 + worn, "your backpack has room for the test gear and what your rings and helmet slots hold (fixture)",
                $"{free} free backpack slot(s), {3 + worn} needed: make room and run it again"))
            return null;
        var helmet = Items.Lookup("Armor_T1_Warrior_DPS_Helmet")?.Id ?? ItemId.None;
        if (!c.Expect(!helmet.IsNone, "the test helmet is in the item catalogue (fixture)", "Armor_T1_Warrior_DPS_Helmet not found"))
            return null;
        var before = new HashSet<EntityId>((Items.Read(p)?.Slots ?? []).Where(s => !s.ItemEntity.IsNone).Select(s => s.ItemEntity));
        foreach (var item in new[] { new ItemId(SetsRingLight), new ItemId(SetsRingShock), helmet })
            await Items.Give(p, item, 1);
        await c.Ticks(10);
        var fresh = (Items.Read(p)?.Slots ?? []).Where(s => !s.ItemEntity.IsNone && !before.Contains(s.ItemEntity) && !s.Ref.IsNone).ToList();
        InventorySlot? New(ItemId item) => fresh.FirstOrDefault(s => s.Item == item);
        var ringA = New(new ItemId(SetsRingLight));
        var ringB = New(new ItemId(SetsRingShock));
        var hat = New(helmet);
        if (!c.Expect(ringA != null && ringB != null && hat != null, "the two test rings and the helmet are in your inventory (fixture)",
                $"ring of light {ringA?.Slot}, ring of shock {ringB?.Slot}, helmet {hat?.Slot}"))
            return null;
        // the targets empty, in one set of removals
        var off = EmptyTargets(p);
        var r = off is null ? null : await Items.EquipSet(p, off);
        await c.Ticks(8);
        var now = Items.CaptureSet(p).Set;
        var empty = now != null && now.Entries.Where(e => IsSetTarget(e.Slot)).All(e => e.Selection is EmptyEquipment);
        if (!c.Expect(r is { Ok: true } && empty, "your rings and helmet slots were emptied first, in one set of removals (fixture)",
                r is null ? "your equipment could not be read" : $"{r.Status} {r.Reason}; empty {empty}"))
            return null;
        var set = new EquipmentSet(now!.Scope,
        [
            new EquipmentSetEntry(EquipmentSlot.Ring0, new InstanceEquipment(ringA!.Ref)),
            new EquipmentSetEntry(EquipmentSlot.Ring1, new InstanceEquipment(ringB!.Ref)),
            new EquipmentSetEntry(EquipmentSlot.ArmourHead, new InstanceEquipment(hat!.Ref)),
        ]);
        var plan = Items.PlanSet(p, set);
        plan.Plan?.Dispose();
        if (!c.Expect(plan.Ok, "the set of two rings and a helmet into the empty slots can be planned", plan.Reason))
            return null;
        return new SetFixture(ringA, ringB, hat, set);
    }

    // The set on: Done, all three worn, every other slot as before, one set event for its ticket.
    private static async Task ApplySet(CheckContext c, EntityId p, SetFixture f, List<EquipmentSetChangedEvent> changed)
    {
        var gear = Items.CaptureSet(p).Set!.Entries;
        var r = await Items.EquipSet(p, f.Set);
        await c.Ticks(8);
        var worn = Items.Equipped(p, EquipmentSlot.Ring0) == f.RingA.Ref && Items.Equipped(p, EquipmentSlot.Ring1) == f.RingB.Ref &&
                   Items.Equipped(p, EquipmentSlot.ArmourHead) == f.Hat.Ref;
        var omitted = Items.CaptureSet(p).Set!.Entries.Where(e => !IsSetTarget(e.Slot)).SequenceEqual(gear.Where(e => !IsSetTarget(e.Slot)));
        var events = changed.Count(e => e.Ticket == r.Ticket);
        c.Expect(r is { Status: EquipmentSetStatus.Done, Committed: true } && worn && omitted && events == 1,
            "the set went on as one transaction: both rings and the helmet worn, every other slot as before, one set event",
            $"{r.Status} {r.Reason}; worn {worn}, other slots unchanged {omitted}, events {events}");
    }


    // Your gear back in two supported sets, each from a fresh capture: the target slots emptied, then what they held at
    // the start put into the empty slots (instance refs stay valid while their items live). Asserted: all 20 slots as
    // in `original`, or the check fails with the reason.
    private static async Task RestoreSet(CheckContext c, EntityId p, EquipmentSet original, Func<EquipmentSlot, bool>? target = null,
        string gear = "rings and helmet")
    {
        var named = target ?? IsSetTarget;
        try
        {
            if (Items.CaptureSet(p).Set is { } now && now.Entries.SequenceEqual(original.Entries))
            {
                c.Expect(true, "your equipment is as it was at the start (nothing to put back)");
                return;
            }
            var off = EmptyTargets(p, named);
            var r1 = off is null ? null : await Items.EquipSet(p, off);
            await SetSettle(c, 8);
            if (!c.Expect(r1 is { Ok: true }, $"putting back 1 of 2: the test {gear} came off in one set of removals",
                    r1 is null ? "your equipment could not be read" : $"{r1.Status} {r1.Reason}"))
                return;
            var scope = Items.CaptureSet(p).Set?.Scope;
            var r2 = scope is null ? null : await Items.EquipSet(p, new EquipmentSet(scope, original.Entries.Where(e => named(e.Slot)).ToList()));
            await SetSettle(c, 8);
            if (!c.Expect(r2 is { Ok: true }, $"putting back 2 of 2: your own {gear} went back into the empty slots",
                    r2 is null ? "your equipment could not be read" : $"{r2.Status} {r2.Reason}"))
                return;
            var end = Items.CaptureSet(p);
            var diff = end.Set is null
                ? end.Reason ?? "not read"
                : string.Join("; ", end.Set.Entries.Zip(original.Entries).Where(x => x.First != x.Second)
                    .Select(x => $"{x.First.Slot} holds {x.First.Selection}, at the start {x.Second.Selection}"));
            c.Expect(end.Set is not null && diff.Length == 0, "your equipment is back as it was at the start (all 20 slots)",
                diff.Length == 0 ? null : diff);
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
        {
            c.Expect(false, "your equipment was put back as it was at the start", $"{e.GetType().Name}: {e.Message}; nothing was substituted");
        }
    }

    // ---- 10-items.equip-set.persist and .persist.after: a set's result through a save and a reload of the character ----

    private static bool IsPersistTarget(EquipmentSlot slot) => slot is EquipmentSlot.Shield or EquipmentSlot.HookShot;

    private const string PersistFile = "equip-set-persist.json";

    // An item's own data as the lab compared it (Durability and Level: their two s32 at +8; ItemState: bytes 8..19,
    // where byte 14 bit 0x08 is "equipped"). Hex per component, null where the item has none. Without ItemState for
    // finding an item again whether it is worn or not.
    private static readonly string[] PersistParts = ["Durability", "Level", "PerkContainer", "PerkContainerNew", "NetworkDurability", "NetworkLevel", "ItemState"];

    private static JsonObject OwnData(EntityId item, bool state = true)
    {
        var o = new JsonObject();
        foreach (var part in PersistParts)
        {
            if (part == "ItemState" && !state) continue;
            var b = World.ReadBytes(item, part);
            var kept = b is null ? null
                : part is "Durability" or "Level" ? (b.Length >= 16 ? b.AsSpan(8, 8).ToArray() : b)
                : part == "ItemState" ? (b.Length >= 20 ? b.AsSpan(8, 12).ToArray() : b)
                : b;
            o[part] = kept is null ? null : Convert.ToHexString(kept);
        }
        return o;
    }

    private static string OwnDataDiff(JsonObject want, JsonObject now) =>
        string.Join("; ", want.Where(kv => (string?)kv.Value != (string?)now[kv.Key])
            .Select(kv => $"{kv.Key} {(string?)now[kv.Key] ?? "none"}, recorded {(string?)kv.Value ?? "none"}"));

    private static EntityId EntityOf(EntityId p, ItemInstanceRef item) =>
        item.IsNone ? EntityId.None : (Items.Read(p)?.Slots ?? []).FirstOrDefault(s => s.Ref == item)?.ItemEntity ?? EntityId.None;

    [ClientCheck("10-items.equip-set.persist", "A set's result is saved with the character (part 1: apply)", Area = "Equipment sets",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = 3202,
        Confirm = "This puts a test shield and a grappling hook on your character and keeps them on until part 2, after you leave " +
                  "and join the world again (your own shield and hook are put back then). Use a disposable character in a copied world.")]
    public async Task EquipmentSetPersist(CheckContext c)
    {
        if (!await NeedPlayer(c)) return;
        var p = c.Player;
        var file = Path.Combine(c.Mod.DataDirectory, PersistFile);
        if (File.Exists(file))
        {
            c.Instruct("Part 1 already ran and waits for part 2: leave the world to the main menu, join it again and run " +
                       "10-items.equip-set.persist.after (it puts your own shield and hook back and clears the record).");
            await c.Skip("part 1 already waits for 10-items.equip-set.persist.after");
            return;
        }
        if (!c.Expect(Capabilities.Is("items.equip_sets"), "equipment sets are on (items.equip_sets)", Capabilities.Why("items.equip_sets")))
        {
            await c.Verdict();
            return;
        }
        var original = Items.CaptureSet(p);
        if (!c.Expect(original is { Ok: true, Set: not null }, "your 20 equipment slots were read (the set to put back in part 2)", original.Reason))
        {
            await c.Verdict();
            return;
        }
        var written = false;
        try
        {
            // what the shield and hook slots hold now, by item type and own data (refs do not outlive the session)
            var originals = new JsonObject();
            foreach (var e in original.Set!.Entries.Where(e => IsPersistTarget(e.Slot)))
                originals[e.Slot.ToString()] = e.Selection switch
                {
                    InstanceEquipment i => new JsonObject { ["item"] = i.Item.Item.Value, ["own"] = OwnData(EntityOf(p, i.Item), state: false) },
                    StackEquipment s => new JsonObject { ["item"] = s.Item.Value },
                    _ => null,
                };
            var worn = originals.Count(kv => kv.Value is not null);
            var free = Items.Read(p)?.FreeSlots() ?? 0;
            if (!c.Expect(free >= 2 + worn, "your backpack has room for the test gear and what your shield and hook slots hold (fixture)",
                    $"{free} free backpack slot(s), {2 + worn} needed: make room and run it again"))
            {
                await c.Verdict();
                return;
            }
            var shieldId = Items.Lookup("Shield_T1_DPS")?.Id ?? ItemId.None;
            var hookId = Items.Lookup("Tool_Grappling_Hook")?.Id ?? ItemId.None;
            if (!c.Expect(!shieldId.IsNone && !hookId.IsNone, "the test shield and grappling hook are in the item catalogue (fixture)",
                    $"Shield_T1_DPS {shieldId}, Tool_Grappling_Hook {hookId}"))
            {
                await c.Verdict();
                return;
            }
            var before = new HashSet<EntityId>((Items.Read(p)?.Slots ?? []).Where(s => !s.ItemEntity.IsNone).Select(s => s.ItemEntity));
            var gear = Items.GearOptionsAvailable ? new Gear(12, ItemRarity.Rare) { Unlocked = 2 } : new Gear(12, ItemRarity.Rare);
            var g1 = await Items.Give(p, shieldId, 1, gear);
            var g2 = await Items.Give(p, hookId, 1);
            await c.Ticks(10);
            var fresh = (Items.Read(p)?.Slots ?? []).Where(s => !s.ItemEntity.IsNone && !before.Contains(s.ItemEntity) && !s.Ref.IsNone).ToList();
            var shield = fresh.FirstOrDefault(s => s.Item == shieldId);
            var hook = fresh.FirstOrDefault(s => s.Item == hookId);
            if (!c.Expect(shield != null && hook != null, "the test shield (level 12, rare) and grappling hook are in your inventory (fixture)",
                    $"shield {g1.Status} {shield?.Slot}, hook {g2.Status} {hook?.Slot}"))
            {
                await c.Verdict();
                return;
            }
            // the hook's durability to a third: data of its own the save has to keep. A tracked write (the save and the
            // players are told); where world.tracked_writes is off the hook keeps its full durability.
            if (World.ReadBytes(hook!.ItemEntity, "Durability") is { Length: >= 16 } dur && BitConverter.ToInt32(dur, 8) is var v && v > 3)
            {
                if (Capabilities.Is(World.TrackedWritesCapability))
                {
                    var w = await World.WriteAt(hook.ItemEntity, "Durability", 8, v / 3);
                    c.Expect(w.Ok, "the grappling hook's durability was lowered to a third (fixture)", w.Reason);
                }
                else
                    c.Log.Info($"client-pass: {c.Id}: world.tracked_writes is off ({Capabilities.Why(World.TrackedWritesCapability)}): the hook keeps its durability");
            }
            // the targets empty, in one set of removals; then both items into the empty slots
            var off = EmptyTargets(p, IsPersistTarget);
            var r0 = off is null ? null : await Items.EquipSet(p, off);
            await c.Ticks(8);
            var now = Items.CaptureSet(p).Set;
            var empty = now != null && now.Entries.Where(e => IsPersistTarget(e.Slot)).All(e => e.Selection is EmptyEquipment);
            if (!c.Expect(r0 is { Ok: true } && empty, "your shield and hook slots were emptied first, in one set of removals (fixture)",
                    r0 is null ? "your equipment could not be read" : $"{r0.Status} {r0.Reason}; empty {empty}"))
            {
                await c.Verdict();
                return;
            }
            var set = new EquipmentSet(now!.Scope,
            [
                new EquipmentSetEntry(EquipmentSlot.Shield, new InstanceEquipment(shield!.Ref)),
                new EquipmentSetEntry(EquipmentSlot.HookShot, new InstanceEquipment(hook.Ref)),
            ]);
            var plan = Items.PlanSet(p, set);
            plan.Plan?.Dispose();
            c.Expect(plan.Ok, "the set of the shield and the hook into the empty slots can be planned", plan.Reason);
            var r = await Items.EquipSet(p, set);
            await c.Ticks(8);
            var on = Items.Equipped(p, EquipmentSlot.Shield) == shield.Ref && Items.Equipped(p, EquipmentSlot.HookShot) == hook.Ref;
            if (!c.Expect(r is { Status: EquipmentSetStatus.Done, Committed: true } && on, "the set put the shield and the hook on (Done)",
                    $"{r.Status} {r.Reason}; worn {on}"))
            {
                await c.Verdict();
                return;
            }
            // the record part 2 compares with after the reload
            var doc = new JsonObject
            {
                ["written"] = DateTime.UtcNow.ToString("O"),
                ["items"] = new JsonObject
                {
                    [nameof(EquipmentSlot.Shield)] = new JsonObject { ["item"] = shieldId.Value, ["own"] = OwnData(shield.ItemEntity) },
                    [nameof(EquipmentSlot.HookShot)] = new JsonObject { ["item"] = hookId.Value, ["own"] = OwnData(hook.ItemEntity) },
                },
                ["originals"] = originals,
            };
            Directory.CreateDirectory(c.Mod.DataDirectory);
            File.WriteAllText(file, doc.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            written = true;
            c.Log.Info($"client-pass: {c.Id}: recorded the set's result in {file}");
            c.Instruct("Part 1 of 2. A test shield (level 12, rare) and a grappling hook with a third of its durability are now worn, " +
                       "put on as one set. Do they show in your equipment? Answer, then leave the world to the main menu (so the " +
                       "game saves your character), host or join the same world again and run 10-items.equip-set.persist.after.");
            await c.Verdict();
        }
        finally
        {
            // only a part 1 that recorded nothing puts your gear back now; otherwise part 2 does
            if (!written) await RestoreSet(c, p, original.Set!, IsPersistTarget, "shield and hook");
        }
    }

    [ClientCheck("10-items.equip-set.persist.after", "A set's result is saved with the character (part 2: after a reload)",
        Area = "Equipment sets", Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = 3203,
        Confirm = "This puts your own shield and grappling hook back after part 1 (10-items.equip-set.persist). Use the character " +
                  "and the world of part 1.")]
    public async Task EquipmentSetPersistAfter(CheckContext c)
    {
        if (!await NeedPlayer(c)) return;
        var p = c.Player;
        var file = Path.Combine(c.Mod.DataDirectory, PersistFile);
        JsonObject? doc = null;
        try { doc = File.Exists(file) ? JsonNode.Parse(File.ReadAllText(file)) as JsonObject : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { c.Log.Info($"client-pass: {c.Id}: {file}: {e.Message}"); }
        if (!c.Expect(doc?["items"] is JsonObject, "part 1 left its record (10-items.equip-set.persist ran first)", file))
        {
            c.Instruct("Run 10-items.equip-set.persist first, then leave the world to the main menu, join it again and run this check.");
            await c.Verdict();
            return;
        }
        if (!c.Expect(Capabilities.Is("items.equip_sets"), "equipment sets are on (items.equip_sets)", Capabilities.Why("items.equip_sets")))
        {
            await c.Verdict();
            return;
        }
        try
        {
            // the same items in the same slots, every recorded byte as before the save
            foreach (var (slotName, node) in (JsonObject)doc!["items"]!)
            {
                var slot = Enum.Parse<EquipmentSlot>(slotName);
                var want = new ItemId((uint)node!["item"]!);
                var item = Items.Equipped(p, slot);
                var entity = EntityOf(p, item);
                if (!c.Expect(item.Item == want && !entity.IsNone, $"{slot} still holds the test item after the reload",
                        $"holds {item.Item}, recorded {want}"))
                    continue;
                var diff = OwnDataDiff((JsonObject)node["own"]!, OwnData(entity));
                c.Expect(diff.Length == 0, $"{slot}: Durability, Level, perks, network copies and ItemState as recorded before the save",
                    diff.Length == 0 ? null : diff);
            }
            c.Instruct("Part 2 of 2. After leaving and joining again: does your character still wear the test shield and the " +
                       "grappling hook, the hook with about a third of its durability and the shield still level 12 and rare? " +
                       "Kiln puts your own shield and hook back once you answer.");
            await c.Verdict();
        }
        finally
        {
            try
            {
                // your own shield and hook: found again in the backpack by item type and own data, put back from fresh plans
                if (Items.CaptureSet(p).Set is { } now)
                {
                    var taken = new HashSet<EntityId>();
                    var entries = now.Entries.ToList();
                    var lost = new List<string>();
                    foreach (var (slotName, node) in doc!["originals"] as JsonObject ?? new JsonObject())
                    {
                        var slot = Enum.Parse<EquipmentSlot>(slotName);
                        EquipmentSetSelection sel = new EmptyEquipment();
                        if (node is JsonObject o)
                        {
                            var id = new ItemId((uint)o["item"]!);
                            var own = o["own"] as JsonObject;
                            var found = (Items.Read(p)?.Slots ?? []).FirstOrDefault(s => s.Item == id && !s.IsEquipped && !s.Ref.IsNone &&
                                !taken.Contains(s.ItemEntity) && (own is null || OwnDataDiff(own, OwnData(s.ItemEntity, state: false)).Length == 0));
                            if (found is null) lost.Add($"{slot}: no {id} with its recorded data in your inventory");
                            else
                            {
                                taken.Add(found.ItemEntity);
                                sel = new InstanceEquipment(found.Ref);
                            }
                        }
                        entries[(int)slot] = new EquipmentSetEntry(slot, sel);
                    }
                    if (c.Expect(lost.Count == 0, "your own shield and hook were found in your inventory", string.Join("; ", lost)))
                        await RestoreSet(c, p, new EquipmentSet(now.Scope, entries), IsPersistTarget, "shield and hook");
                }
                else
                    c.Expect(false, "your equipment was read for putting your own shield and hook back", "not read");
            }
            catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
            {
                c.Expect(false, "your own shield and hook were put back", $"{e.GetType().Name}: {e.Message}");
            }
            try { File.Delete(file); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
    }
}
