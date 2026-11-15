// SPDX-License-Identifier: MIT
// equipment-sets-test, the rollback series (kiln/data/equipment-sets-test/series with kiln/data/kiln.items/sets-lab and
// sets-matrix): does the game's rollback leave an item's own data alone? One spawned 1_Player stand-in (saved with the
// world: no Transient flag) carries items with real instance data: two shields with a level and unlocked perks, a sword
// with a level and a perk, a grappling hook and a pickaxe whose durability is lowered, a watering can with its fuel set.
// For every enabled form of a plan (empty targets, removals, the one swap, plain stacks, arrows in place, the cycle
// through a free slot, and a form mixing them) a failure is injected at EVERY operation (the game's own equip refused at
// step k), after the last one (the commit's rollback bit: refused before the commit), at the commit (committed with the
// bit: the game's commit rolls back by itself) and as a dry run. After each, two comparisons must hold:
//   - kiln.items' own (native, in the same program update): the canonical snapshot and every byte of the seven
//     components its program declares (Fuel, Inventory, InventoryEmptyState, InventorySetup, ItemState,
//     OwnerRelationship, SlotSelection) of every entity of the player's tree, read before the transaction and after the
//     rollback: a Refused, CommitRolledBack or Simulated status is that comparison passing (a difference is
//     InvariantFailure, and sets-matrix keeps sets on to go on with the series);
//   - this test's (tick thread, before the call and after the result): every byte of those seven components and of the
//     item components the program cannot write (Durability, Level, PerkContainer, PerkContainerNew, NetworkDurability,
//     NetworkFuel, NetworkLevel, ItemEquippedLifetime) of the player, every inventory and every item entity.
// Then the form is committed: Done, each slot as asked, and every item's own bytes as before (it may have moved).
// At the end the stand-in's state is written out (entity ids replaced by places), the world saved, and the run stops
// there; started again (the file is there), the test looks for the stand-in to compare the same and removes both. Lab
// 2026-11-15 (server 26430253): the game does not load a spawned 1_Player from a dedicated server's world save (a
// character is its client's own), so SERIES-RESTART fails there by design of the game; a set's result over a save and a
// reload of a real character is the guided checks 10-items.equip-set.persist and .persist.after.
using System.Text;
using Kiln;

namespace EquipmentSetsTest;

public sealed partial class EquipmentSetsTestMod
{
    private static readonly string[] Written = ["Fuel", "Inventory", "InventoryEmptyState", "InventorySetup", "ItemState", "OwnerRelationship", "SlotSelection"];
    private static readonly string[] ItemOwn = ["Durability", "Level", "PerkContainer", "PerkContainerNew", "NetworkDurability", "NetworkFuel", "NetworkLevel", "ItemEquippedLifetime"];
    private const string SeriesTag = "equipment-sets-series";

    // the entities of the player's tree: the player, every inventory it links, every item entity in them
    private static List<EntityId> TreeOf(EntityId p)
    {
        var c = Items.Read(p);
        var all = new List<EntityId> { p };
        if (c is null) return all;
        all.AddRange(c.Inventories.Select(i => i.Inventory));
        all.AddRange(c.Slots.Select(s => s.Slot.Inventory));
        all.AddRange(c.Slots.Where(s => !s.ItemEntity.IsNone).Select(s => s.ItemEntity));
        return all.Where(e => !e.IsNone).Distinct().OrderBy(e => e.Value).ToList();
    }

    // every byte of every compared component of every entity of the tree (null: the entity has none)
    private static Dictionary<(uint, string), byte[]?> Raw(EntityId p)
    {
        var d = new Dictionary<(uint, string), byte[]?>();
        foreach (var e in TreeOf(p))
            foreach (var part in Written.Concat(ItemOwn))
                d[(e.Value, part)] = World.ReadBytes(e, part);
        return d;
    }

    private static string RawDiff(Dictionary<(uint, string), byte[]?> a, Dictionary<(uint, string), byte[]?> b)
    {
        var out_ = new List<string>();
        foreach (var k in a.Keys.Union(b.Keys).OrderBy(k => k.Item1).ThenBy(k => k.Item2))
        {
            var x = a.GetValueOrDefault(k);
            var y = b.GetValueOrDefault(k);
            if (x is null && y is null) continue;
            if (x is null || y is null) out_.Add($"{k.Item2} of {k.Item1} {(x is null ? "appeared" : "is gone")}");
            else if (!x.AsSpan().SequenceEqual(y))
            {
                var at = 0;
                while (at < Math.Min(x.Length, y.Length) && x[at] == y[at]) at++;
                out_.Add($"{k.Item2} of {k.Item1} at byte {at}: {Convert.ToHexString(x)} -> {Convert.ToHexString(y)}");
            }
        }
        return out_.Count == 0 ? "" : string.Join("; ", out_.Take(6)) + (out_.Count > 6 ? $" (+{out_.Count - 6} more)" : "");
    }

    // An item entity's own bytes: everything but where it lies. Equipping changes three things on purpose, checked apart
    // and exactly (Placement): ItemState's first 8 bytes (its slot), ItemState's equipped bit (0x08 of the itemState mask,
    // byte 14) and OwnerRelationship (the inventory entity that holds the item, the backpack's or an equipment page's).
    private static string OwnOf(EntityId item)
    {
        var sb = new StringBuilder();
        foreach (var part in ItemOwn.Append("Fuel").Append("ItemState"))
        {
            var b = World.ReadBytes(item, part);
            if (b is null) continue;
            if (part == "ItemState")
            {
                if (b.Length < 20) continue;
                b = b[8..];
                b[6] &= 0xF7;  // the equipped bit (byte 14 of the component)
            }
            sb.Append(part).Append('=').Append(Convert.ToHexString(b)).Append(' ');
        }
        return sb.ToString();
    }

    private static (uint Owner, uint Inventory) OwnerOf(EntityId p, EntityId item) =>
        (World.ReadBytes(item, "OwnerRelationship") is { Length: >= 4 } o ? BitConverter.ToUInt32(o, 0) : 0,
         Items.Read(p)?.Slots.FirstOrDefault(s => s.ItemEntity == item)?.Slot.Inventory.Value ?? 0);

    // after a commit: each item's equipped bit says whether it is worn, and an owner that named the item's inventory names
    // the one that holds it now (one that named anything else is unchanged); "" when all hold
    private static string Placement(EntityId p, Dictionary<EntityId, (uint Owner, uint Inventory)> before)
    {
        var bad = new List<string>();
        foreach (var s in Items.Read(p)!.Slots.Where(s => !s.ItemEntity.IsNone))
        {
            if (World.ReadBytes(s.ItemEntity, "ItemState") is { Length: >= 20 } st && ((st[14] & 0x08) != 0) != s.IsEquipped)
                bad.Add($"{s.ItemEntity}: equipped bit {(st[14] & 0x08) != 0}, worn {s.IsEquipped}");
            if (!before.TryGetValue(s.ItemEntity, out var was)) continue;
            var (owner, _) = OwnerOf(p, s.ItemEntity);
            var want = was.Owner == was.Inventory ? s.Slot.Inventory.Value : was.Owner;
            if (owner != want) bad.Add($"{s.ItemEntity}: owner {owner}, expected {want} (was {was.Owner} in {was.Inventory})");
        }
        return string.Join("; ", bad);
    }

    // The tree with entity ids replaced by places (inventory ordinal and slot index), for a comparison over a restart.
    // Durability, Fuel and Level: their stored values (bytes 8..15); their first 8 bytes are not compared over a restart.
    private static string Placed(EntityId p)
    {
        var c = Items.Read(p);
        if (c is null) return "unreadable";
        var invs = c.Inventories.Select(i => i.Inventory).Concat(c.Slots.Select(s => s.Slot.Inventory)).Distinct().ToList();
        string Place(ulong slotId)
        {
            if (slotId == 0) return "-";
            var inv = new EntityId((uint)(slotId & 0xFFFFFFFF));
            var i = invs.IndexOf(inv);
            return i < 0 ? $"other:{(slotId >> 32) & 0xFF}" : $"{i}:{(slotId >> 32) & 0xFF}";
        }
        string Who(uint e) => e == p.Value ? "player" : invs.IndexOf(new EntityId(e)) is >= 0 and var i ? $"inv{i}" : e == 0 ? "none" : "other";
        var sb = new StringBuilder();
        sb.Append($"links {string.Join(",", c.Inventories.Select(i => $"{invs.IndexOf(i.Inventory)}/{i.Category}"))} slots {c.GenericSlots}/{c.AvailableSlots}\n");
        if (World.ReadBytes(p, "SlotSelection") is { Length: >= 176 } sel)
        {
            sb.Append("equipment");
            for (var x = 0; x < 20; x++) sb.Append(' ').Append(Place(BitConverter.ToUInt64(sel, x * 8)));
            sb.Append($"\nactionbars {Who(BitConverter.ToUInt32(sel, 160))} {Who(BitConverter.ToUInt32(sel, 164))}\n");
        }
        foreach (var s in c.Slots.Where(s => !s.IsEmpty).OrderBy(s => invs.IndexOf(s.Slot.Inventory)).ThenBy(s => s.Slot.Index))
        {
            sb.Append($"{invs.IndexOf(s.Slot.Inventory)}:{s.Slot.Index} {s.Item.Value:x8}x{s.Count}{(s.IsEquipped ? "*" : "")}");
            if (!s.ItemEntity.IsNone)
            {
                foreach (var part in ItemOwn.Append("Fuel"))
                {
                    var b = World.ReadBytes(s.ItemEntity, part);
                    if (b is null) continue;
                    var kept = part is "Durability" or "Fuel" or "Level" && b.Length >= 16 ? b.AsSpan(8, 8) : b;
                    sb.Append($" {part}={Convert.ToHexString(kept)}");
                }
                if (World.ReadBytes(s.ItemEntity, "ItemState") is { Length: >= 20 } st)
                    sb.Append($" ItemState={Place(BitConverter.ToUInt64(st, 0))}/{Convert.ToHexString(st.AsSpan(8))}");
                if (World.ReadBytes(s.ItemEntity, "OwnerRelationship") is { Length: >= 4 } ow) sb.Append($" owner={Who(BitConverter.ToUInt32(ow, 0))}");
            }
            sb.Append('\n');
        }
        return sb.ToString();
    }

    private async Task<ItemsResult?> GiveSome(EntityId p, string name, int count = 1, Gear? gear = null)
    {
        if (Items.Lookup(name) is not { } info)
        {
            Log.Info($"equipment-sets-test: SERIES: no item {name} in this build");
            return null;
        }
        var r = await Items.Give(p, info.Id, count, gear is { } g && !Items.GearOptionsAvailable ? new Gear(g.Level, g.Rarity) : gear);
        if (r.Status != ItemsStatus.Done) Log.Info($"equipment-sets-test: SERIES: give {name}: {r.Status} {r.Message}");
        return r;
    }

    // lower a durability-like value (Durability / Fuel / Level dataStorage at +8: two s32) to a third of what it is
    private static void Lower(EntityId item, string part)
    {
        if (World.ReadBytes(item, part) is not { Length: >= 16 } b) return;
        var v = BitConverter.ToInt32(b, 8);
        if (v > 3) World.WriteRaw(item, part, 8, v / 3);
    }

    private async Task Series(WorldPosition at)
    {
        var file = Path.Combine(Context.DataDirectory, "series-end.txt");
        if (File.Exists(file))
        {
            await SeriesAfterRestart(file);
            return;
        }
        var p = await Spawn.Entity("1_Player", at, 0, new SpawnOptions { Tag = SeriesTag });  // saved with the world
        await Steps(40);
        Check(World.Has(p, "PlayerInput") && Items.Read(p) is { Initialized: true }, $"SERIES: a spawned player character {p} (saved with the world)");
        foreach (var name in (string[])["Ring_T3_Ring_of_Light", "Ring_T6_Ring_of_Shock", "Armor_T1_Warrior_DPS_Helmet", "Armor_T1_Ranger_Archer_DPS_Helmet"])
            await GiveSome(p, name);
        await GiveSome(p, "Ammo_T1_Arrow_Flint_lvl5", 20);
        await GiveSome(p, "Ammo_T1_Arrow_Scrap_lvl10", 15);
        await GiveSome(p, "Shield_T1_DPS", 1, new Gear(12, ItemRarity.Rare) { Unlocked = 2 });
        await GiveSome(p, "Shield_T7_DPS", 1, new Gear(30, ItemRarity.Epic) { Unlocked = 3 });
        await GiveSome(p, "Weapon_T1_1H_SwordEpic_01", 1, new Gear(20) { Unlocked = 1 });
        await GiveSome(p, "Tool_Grappling_Hook");
        await GiveSome(p, "Tool_T1_Pickaxe_Scrap_Grassland");
        await GiveSome(p, "Tool_T1_WateringCan");
        await Steps(6);
        ItemId Id2(string n) => Items.Lookup(n)?.Id ?? ItemId.None;
        foreach (var s in Items.Read(p)!.Slots.Where(s => !s.ItemEntity.IsNone))
            if (s.Item == Id2("Tool_Grappling_Hook") || s.Item == Id2("Tool_T1_Pickaxe_Scrap_Grassland")) Lower(s.ItemEntity, "Durability");
        if (Items.Fuel.Available && !Id2("Tool_T1_WateringCan").IsNone) await Items.Fuel.SetAll(p, 2500, Id2("Tool_T1_WateringCan"));
        await Steps(4);
        Log.Info($"equipment-sets-test: SERIES: the stand-in's items with their own data:\n{Placed(p)}");

        var light = Id2("Ring_T3_Ring_of_Light");
        var shock = Id2("Ring_T6_Ring_of_Shock");
        var helmetA = Id2("Armor_T1_Warrior_DPS_Helmet");
        var shieldA = Id2("Shield_T1_DPS");
        var shieldB = Id2("Shield_T7_DPS");
        var hook = Id2("Tool_Grappling_Hook");
        var flint = Id2("Ammo_T1_Arrow_Flint_lvl5");
        var scrap = Id2("Ammo_T1_Arrow_Scrap_lvl10");
        EquipmentSetSelection Pick(ItemId item) =>
            Loose(p, item) is { } s ? (s.Ref.IsNone ? new StackEquipment(s.Item, s.Slot, s.Count) : new InstanceEquipment(s.Ref)) : new EmptyEquipment();
        EquipmentSlot[] touched = [EquipmentSlot.ArmourHead, EquipmentSlot.Ring0, EquipmentSlot.Ring1, EquipmentSlot.Shield, EquipmentSlot.Arrow, EquipmentSlot.HookShot];
        var forms = new (string Name, (ItemId Item, EquipmentSlot Slot)[] Pre, Func<(EquipmentSlot, EquipmentSetSelection)[]> Entries)[]
        {
            ("empty targets (a perked shield, a worn grappling hook, a ring, a helmet)", [],
                () => [(EquipmentSlot.ArmourHead, Pick(helmetA)), (EquipmentSlot.Ring1, Pick(light)), (EquipmentSlot.Shield, Pick(shieldA)), (EquipmentSlot.HookShot, Pick(hook))]),
            ("removals (the shield, the hook, a ring)", [(shieldA, EquipmentSlot.Shield), (hook, EquipmentSlot.HookShot), (light, EquipmentSlot.Ring0)],
                () => [(EquipmentSlot.Shield, new EmptyEquipment()), (EquipmentSlot.HookShot, new EmptyEquipment()), (EquipmentSlot.Ring0, new EmptyEquipment())]),
            ("the one swap (a perked shield for another)", [(shieldA, EquipmentSlot.Shield)], () => [(EquipmentSlot.Shield, Pick(shieldB))]),
            ("plain stacks (arrows into the empty arrow slot)", [], () => [(EquipmentSlot.Arrow, Pick(flint))]),
            ("arrows in place (an arrow swap)", [(flint, EquipmentSlot.Arrow)], () => [(EquipmentSlot.Arrow, Pick(scrap))]),
            ("the cycle through a free slot (two worn rings exchanged)", [(light, EquipmentSlot.Ring0), (shock, EquipmentSlot.Ring1)],
                () => [(EquipmentSlot.Ring0, new InstanceEquipment(Items.Equipped(p, EquipmentSlot.Ring1))), (EquipmentSlot.Ring1, new InstanceEquipment(Items.Equipped(p, EquipmentSlot.Ring0)))]),
            ("mixed: a removal, an empty target, the one swap, arrows", [(helmetA, EquipmentSlot.ArmourHead), (shieldA, EquipmentSlot.Shield), (flint, EquipmentSlot.Arrow)],
                () => [(EquipmentSlot.ArmourHead, new EmptyEquipment()), (EquipmentSlot.Ring0, Pick(light)), (EquipmentSlot.Shield, Pick(shieldB)), (EquipmentSlot.Arrow, Pick(scrap))]),
        };
        foreach (var (name, pre, entries) in forms)
        {
            if (_stop) return;
            // from a clean start: the touched slots emptied (removals only), then the form's own gear worn one by one
            var clear = Items.CaptureSet(p).Set!;
            var off = clear.Entries.Where(e => touched.Contains(e.Slot) && e.Selection is not EmptyEquipment).Select(e => new EquipmentSetEntry(e.Slot, new EmptyEquipment())).ToArray();
            if (off.Length > 0) await Items.EquipSet(p, clear with { Entries = off });
            await Steps(1);
            foreach (var (item, slot) in pre)
            {
                if (Loose(p, item) is { } s) await Items.Equip(p, s.Slot, slot);
                await Steps(1);
            }
            var set = new EquipmentSet(Items.CaptureSet(p).Set!.Scope, entries().Select(e => new EquipmentSetEntry(e.Item1, e.Item2)).ToArray());
            var plan = Items.PlanSet(p, set);
            var n = plan.Plan?.Steps.Count ?? 0;
            plan.Plan?.Dispose();
            if (!plan.Ok)
            {
                Check(false, $"SERIES {name}: the form does not plan ({plan.Reason})");
                continue;
            }
            async Task Injected(string what, Func<Task<EquipmentSetResult>> run, EquipmentSetStatus expect, int? failed = null)
            {
                var before = Raw(p);
                var gear = Gear(p);
                var r = await run();
                await Steps(1);
                var diff = RawDiff(before, Raw(p));
                Check(r.Status == expect && (failed is null || r.FailedOperation == failed) && !r.MayHaveMutated && diff.Length == 0 && Gear(p) == gear,
                    $"SERIES {name}: {what}: {expect}, kiln.items' full comparison and this test's read of every component equal{(diff.Length > 0 ? $" [DIFF {diff}]" : "")} [{Show(r)}]");
            }
            for (var k = 0; k < n; k++)
                await Injected($"the game refused operation {k} of {n}", () => Items.LabEquipSet(p, set, LabSetFault.RefuseStep, k), EquipmentSetStatus.Refused, k);
            await Injected($"after all {n} operations, the commit's rollback bit (refused before the commit)", () => Items.LabEquipSet(p, set, LabSetFault.GuardBit), EquipmentSetStatus.Refused);
            await Injected("committed with the bit set (the game's commit rolls back by itself)", () => Items.LabEquipSet(p, set, LabSetFault.CommitAnyway), EquipmentSetStatus.CommitRolledBack);
            await Injected("a dry run", () => Items.EquipSet(p, set, EquipOptions.DryRun), EquipmentSetStatus.Simulated);
            // committed: every item's own bytes as before, wherever it went
            var held = Items.Read(p)!.Slots.Where(s => !s.ItemEntity.IsNone).ToList();
            var own = held.ToDictionary(s => s.ItemEntity, s => OwnOf(s.ItemEntity));
            var owners = held.ToDictionary(s => s.ItemEntity, s => OwnerOf(p, s.ItemEntity));
            var done = await Items.EquipSet(p, new EquipmentSet(Items.CaptureSet(p).Set!.Scope, set.Entries));
            await Steps(1);
            var moved = own.Where(kv => OwnOf(kv.Key) != kv.Value).Select(kv => $"{kv.Key}: {kv.Value} -> {OwnOf(kv.Key)}").ToList();
            var placed = Placement(p, owners);
            Check(done.Status == EquipmentSetStatus.Done && Holds(p, set) && moved.Count == 0 && placed.Length == 0,
                $"SERIES {name}: committed ({n} operations): Done, as asked, every item's own bytes as before, its equipped bit and owner where it lies" +
                $"{(moved.Count > 0 ? $" [CHANGED {string.Join("; ", moved)}]" : "")}{(placed.Length > 0 ? $" [PLACED {placed}]" : "")} [{Show(done)}]");
        }

        // the end state, the save, and the restart (run again with the file there)
        var end = Placed(p);
        await File.WriteAllTextAsync(file, $"{p.Value}\n{end}");
        var saved = await Saves.Request();
        Check(saved.Ok, $"SERIES: the world saved with the stand-in ({saved.Reason} revision {saved.Revision}: {saved.Error}); restart the server for SERIES-RESTART");
        Log.Info($"equipment-sets-test: SERIES-SAVED: the stand-in {p}:\n{end}");
    }

    private async Task SeriesAfterRestart(string file)
    {
        var lines = await File.ReadAllLinesAsync(file);
        var was = string.Join("\n", lines.Skip(1)) + "\n";
        await Steps(60);
        var found = World.Find("SlotSelection").Where(e => World.Has(e, "PlayerInput") && Items.Read(e) is { Initialized: true }).ToList();
        if (found.Count == 0)
        {
            Check(false, "SERIES-RESTART: no player character after the restart: the game did not load the spawned stand-in from the save");
            File.Delete(file);
            return;
        }
        var best = found.Select(e => (e, Placed(e))).OrderByDescending(x => x.Item2 == was).First();
        var now = best.Item2;
        var a = was.Split('\n');
        var b = now.Split('\n');
        var diffs = a.Zip(b).Where(x => x.First != x.Second).Select(x => $"was [{x.First}] now [{x.Second}]").ToList();
        if (a.Length != b.Length) diffs.Add($"{a.Length} lines before, {b.Length} now");
        Check(diffs.Count == 0, $"SERIES-RESTART: the stand-in {best.e} (was {lines[0]}) after a save and a restart: every slot, item and item's own data as before the save" +
                                (diffs.Count > 0 ? $" [{string.Join("; ", diffs.Take(8))}]" : $" ({b.Length - 3} items)"));
        Log.Info($"equipment-sets-test: SERIES-RESTART: now:\n{now}");
        File.Delete(file);
        Spawn.Destroy(best.e);
        await Steps(3);
    }
}
