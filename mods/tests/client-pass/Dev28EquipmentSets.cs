// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 10-equipment-sets (0.2.0-dev.29): the guided check of chapter 10. Orders 3200-3299 (lab port 28120).
//
// - 10-items.equip-set: one PlayerEquippingEvent decides a whole set (a veto changes nothing), a dry run changes nothing,
//   and an allowed set of two rings and a helmet appears on the player at once: the player's own client and a second
//   observer see no ring alone for a moment and no ghost item in the bag. items.equip_sets ships on in 0.2.0-dev.29
//   without this check (nothing ships held); the lab (mods/tests/equipment-sets-test) ran the machinery on a spawned
//   character, never on a player's client. The player's equipment is put back at the end from a fresh plan of what it
//   held at the start (never a guessed substitute: a set that cannot be put back is reported).
public sealed partial class ClientPassMod
{
    private const uint SetsRingLight = 0x79442800;  // Ring_T3_Ring_of_Light
    private const uint SetsRingShock = 0x4623bcb0;  // Ring_T6_Ring_of_Shock

    [ClientCheck("10-items.equip-set", "One decision and visible atomic set swap", Area = "Equipment sets", Since = Dev29,
        Needs = ClientCheckNeeds.Hosted, Tags = [Visual], Order = 3200,
        Confirm = "This changes your character's rings and helmet for a while and gives you two rings and a helmet (your gear is put " +
                  "back at the end). Use a disposable character in a copied world.")]
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
        var changed = new List<EquipmentSetChangedEvent>();
        var veto = false;
        var subs = new List<IDisposable>
        {
            Events.On<PlayerEquippingEvent>(e =>
            {
                if (e.Change is not SetEquipmentChange) return;
                decisions.Add(e);
                if (veto) e.Cancel();
            }, EventFilter.ForEntity(p)),
            Events.On<EquipmentSetChangedEvent>(changed.Add, EventFilter.ForEntity(p)),
        };
        c.OnEnd(() =>
        {
            foreach (var s in subs) s.Dispose();
        });
        try
        {
            // the test gear: two rings and a helmet the set will put on (the game may wear what fits an empty slot at once)
            var helmet = Items.Lookup("Armor_T1_Warrior_DPS_Helmet")?.Id ?? ItemId.None;
            var given = new List<ItemId> { new(SetsRingLight), new(SetsRingShock), helmet };
            var before = new HashSet<EntityId>((Items.Read(p)?.Slots ?? []).Where(s => !s.ItemEntity.IsNone).Select(s => s.ItemEntity));
            foreach (var item in given)
                if (!item.IsNone) await Items.Give(p, item, 1);
            await c.Ticks(10);
            var fresh = (Items.Read(p)?.Slots ?? []).Where(s => !s.ItemEntity.IsNone && !before.Contains(s.ItemEntity) && !s.Ref.IsNone).ToList();
            InventorySlot? New(ItemId item) => fresh.FirstOrDefault(s => s.Item == item);
            var ringA = New(new ItemId(SetsRingLight));
            var ringB = New(new ItemId(SetsRingShock));
            var hat = helmet.IsNone ? null : New(helmet);
            if (!c.Expect(ringA != null && ringB != null && hat != null, "the two test rings and the helmet are in your inventory (fixture)",
                    $"ring of light {ringA?.Slot}, ring of shock {ringB?.Slot}, helmet {hat?.Slot}"))
            {
                await c.Verdict();
                return;
            }
            var now = Items.CaptureSet(p).Set!;
            var set = new EquipmentSet(now.Scope,
            [
                new EquipmentSetEntry(EquipmentSlot.Ring0, new InstanceEquipment(ringA!.Ref)),
                new EquipmentSetEntry(EquipmentSlot.Ring1, new InstanceEquipment(ringB!.Ref)),
                new EquipmentSetEntry(EquipmentSlot.ArmourHead, new InstanceEquipment(hat!.Ref)),
            ]);
            var plan = Items.PlanSet(p, set);
            plan.Plan?.Dispose();
            c.Expect(plan.Ok, "the set of two rings and a helmet can be planned", plan.Reason);

            // 1. a veto: one decision for the whole set, nothing changes
            c.Instruct("Step 1 of 3: Kiln asks to put two rings and a helmet on you as one set, and a test handler refuses it. Nothing " +
                       "should change on your character or in your inventory.");
            var gear = Items.CaptureSet(p).Set!.Entries;
            veto = true;
            var r1 = await Items.EquipSet(p, set);
            veto = false;
            c.Expect(r1 is { Status: EquipmentSetStatus.Canceled, Committed: false, Changes.Count: 0 } &&
                     decisions.Count(e => e.Change is SetEquipmentChange s && s.ItemsTicket == r1.Ticket) == 1 &&
                     Items.CaptureSet(p).Set!.Entries.SequenceEqual(gear),
                "one decision for the three slots, refused: nothing changed", $"{r1.Status} {r1.Reason}; decisions {decisions.Count}");

            // 2. a dry run: nothing changes
            var r2 = await Items.EquipSet(p, set, EquipOptions.DryRun);
            c.Expect(r2 is { Status: EquipmentSetStatus.Simulated, DryRun: true, Committed: false, Changes.Count: 0 } &&
                     changed.All(e => e.Ticket != r2.Ticket) && Items.CaptureSet(p).Set!.Entries.SequenceEqual(gear),
                "a dry run of the set changed nothing", $"{r2.Status} {r2.Reason}");
            await c.Ticks(30);

            // 3. allowed: the three slots at once
            c.Instruct("Step 2 of 3: open your inventory or character screen now and keep watching your rings and helmet. In a few " +
                       "seconds Kiln puts the set on in one transaction: both rings and the helmet should appear at the same moment.");
            await c.Ticks(150);
            var r3 = await Items.EquipSet(p, set);
            await c.Ticks(8);
            var worn = Items.Equipped(p, EquipmentSlot.Ring0) == ringA.Ref && Items.Equipped(p, EquipmentSlot.Ring1) == ringB.Ref &&
                       Items.Equipped(p, EquipmentSlot.ArmourHead) == hat.Ref;
            var omitted = Items.CaptureSet(p).Set!.Entries.Where(e => e.Slot is not (EquipmentSlot.Ring0 or EquipmentSlot.Ring1 or EquipmentSlot.ArmourHead))
                .SequenceEqual(gear.Where(e => e.Slot is not (EquipmentSlot.Ring0 or EquipmentSlot.Ring1 or EquipmentSlot.ArmourHead)));
            c.Expect(r3 is { Status: EquipmentSetStatus.Done, Committed: true } && worn && omitted && changed.Count(e => e.Ticket == r3.Ticket) == 1,
                "the set went on as one transaction: both rings and the helmet worn, every other slot as before, one set event",
                $"{r3.Status} {r3.Reason}; worn {worn}, other slots unchanged {omitted}, events {changed.Count(e => e.Ticket == r3.Ticket)}");
            c.Instruct("Step 3 of 3: did both rings and the helmet appear at the same time, with no moment where only one ring was worn, " +
                       "an item showed twice, or an item flashed in the bag? If a second player is joined (best one without Kiln), ask " +
                       "what they saw on your character. Pass only if nothing showed an in-between state; a ghost item or a ring that " +
                       "shows on your screen but not on theirs is a fail.");
            await c.Verdict();
        }
        finally
        {
            // your gear back, from a fresh plan of what you wore at the start (never a substitute)
            try
            {
                var back = await Items.EquipSet(p, original.Set!);
                c.Log.Info($"client-pass: {c.Id}: put back the equipment you had at the start: {back.Status}{(back.Reason is { } why ? $" ({why})" : "")}");
            }
            catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
            {
                c.Log.Info($"client-pass: {c.Id}: your equipment could not be put back ({e.Message}); nothing was substituted");
            }
        }
    }
}
