// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Equipment wave P7 (0.2.0-dev.29.19): per-instance values (Items.SetPower, SetArmor, SetDurabilityMax, SetLevel with
// AnyLevel). A hosted world: the hosting game is the server, so the values act there. Orders 2970-2979.
//
// - equip-p7.power: hits with a sword at power x1, then x3 on the same item: the damage Kiln.Combat reports for the
//   player's hits on one target grows about threefold (armour and resistances act after, so not exactly).
// - equip-p7.armor: the damage the player takes from one enemy in a chest piece at armour x1 and at x10.
// - equip-p7.tooltip: a Kiln client's item detail shows the sword's damage number x2 (ui.tooltip.values); asks also what the
//   level line and the durability bar show after a level outside the type's range and a max durability.
// - equip-p7.reload: after the world is saved, closed and hosted again, the level, max durability and multipliers are back.
public sealed partial class ClientPassMod
{
    private const string ValuesSword = "Weapon_T1_1H_Sword_Craft";      // Common, level 5, damage set
    private const string ValuesChest = "Armor_T2_Warrior_Tank_Chest";   // Rare, level 10, armour

    private static async Task<ItemInstanceRef> GiveHeldLike(CheckContext c, string item, List<(ItemId Item, int Count)> given)
    {
        var g = await Items.Give(c.Player, item, 1);
        if (g.Succeeded) given.Add((g.Item, 1));
        c.Expect(g.Succeeded, $"{item} given", g.ToString());
        return g.Changes.FirstOrDefault(x => !x.Ref.IsNone).Ref;
    }

    [ClientCheck("equip-p7.power", "A sword's own power multiplier changes the damage it deals", Area = GearArea, Since = "0.2.0-dev.29.19",
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2970,
        Confirm = "This gives you a sword and asks you to hit a creature with it. Use a disposable character in a copied world.")]
    public async Task ValuesPower(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, "items.gear.power")) return;
        var given = new List<(ItemId, int)>();
        var sword = await GiveHeldLike(c, ValuesSword, given);
        if (sword.IsNone)
        {
            await c.Verdict();
            return;
        }
        var hits = new List<(int Phase, EntityId Target, int Amount)>();
        var phase = 1;
        var sub = Events.On<EntityDamagedEvent>(e =>
        {
            if (e.Attacker == c.Player && e.Amount > 0) lock (hits) hits.Add((phase, e.Target, e.Amount));
        });
        c.OnEnd(() =>
        {
            sub.Dispose();
            if (Items.ValuesOf(sword) is not null) Items.SetPower(sword, 1);
        });
        c.Instruct($"Equip the new sword ({ValuesSword}) and hit one creature (a training dummy or a weak enemy) three times. Press continue.");
        await c.Verdict();
        var p = Items.SetPower(sword, 3);
        c.Expect(p.Ok, "the sword's power x3", p.ToString());
        phase = 2;
        c.Instruct("Hit the same creature three more times with the same sword. Press continue.");
        await c.Verdict();
        List<(int Phase, EntityId Target, int Amount)> seen;
        lock (hits) seen = [.. hits];
        double Avg(int ph) => seen.Where(h => h.Phase == ph).Select(h => (double)h.Amount).DefaultIfEmpty(0).Average();
        var (a1, a3) = (Avg(1), Avg(2));
        c.Expect(a1 > 0 && a3 > 0, "hits seen in both phases", $"{seen.Count(h => h.Phase == 1)} then {seen.Count(h => h.Phase == 2)} hits");
        c.Expect(a1 > 0 && a3 >= 2 * a1, "the damage grew about threefold", $"average {a1:0.#} at x1, {a3:0.#} at x3");
        c.Instruct("Did the damage numbers you saw (or the creature's health bar) drop clearly faster in the second round?");
        await c.Verdict();
        foreach (var (item, count) in given) await Items.Take(c.Player, item, count);
    }

    [ClientCheck("equip-p7.armor", "A chest piece's own armour multiplier changes the damage the wearer takes", Area = GearArea,
        Since = "0.2.0-dev.29.19", Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2971,
        Confirm = "This gives you a chest piece and asks you to let one creature hit you. Use a disposable character in a copied world.")]
    public async Task ValuesArmor(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, "items.gear.armor")) return;
        var given = new List<(ItemId, int)>();
        var chest = await GiveHeldLike(c, ValuesChest, given);
        if (chest.IsNone)
        {
            await c.Verdict();
            return;
        }
        var taken = new List<(int Phase, int Amount)>();
        var phase = 1;
        var sub = Events.On<EntityDamagedEvent>(e =>
        {
            if (e.Target == c.Player && e.Amount > 0 && !e.Attacker.IsNone && e.Attacker != c.Player) lock (taken) taken.Add((phase, e.Amount));
        });
        c.OnEnd(() =>
        {
            sub.Dispose();
            if (Items.ValuesOf(chest) is not null) Items.SetArmor(chest, 1);
        });
        c.Instruct($"Wear the new chest piece ({ValuesChest}) and let one weak creature hit you three times. Press continue.");
        await c.Verdict();
        var a = Items.SetArmor(chest, 10);
        c.Expect(a.Ok, "the chest piece's armour x10", a.ToString());
        phase = 2;
        c.Instruct("Let the same creature hit you three more times. Press continue.");
        await c.Verdict();
        List<(int Phase, int Amount)> seen;
        lock (taken) seen = [.. taken];
        double Avg(int ph) => seen.Where(h => h.Phase == ph).Select(h => (double)h.Amount).DefaultIfEmpty(0).Average();
        c.Expect(Avg(1) > 0 && Avg(2) > 0 && Avg(2) < Avg(1), "the damage taken went down", $"average {Avg(1):0.#} at x1, {Avg(2):0.#} at x10");
        c.Instruct("Did you lose clearly less health per hit in the second round? Does the character screen's armour value change too (say what it shows)?");
        await c.Verdict();
        foreach (var (item, count) in given) await Items.Take(c.Player, item, count);
    }

    [ClientCheck("equip-p7.tooltip", "The item detail shows a multiplied sword's damage, a level past its range and a longer durability",
        Area = GearArea, Since = "0.2.0-dev.29.19", Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2972,
        Confirm = "This gives you a sword for a moment and takes it back. Use a disposable character in a copied world.")]
    public async Task ValuesTooltip(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, "items.gear.power", Ui.Tooltip.ValuesCapability)) return;
        var given = new List<(ItemId, int)>();
        var sword = await GiveHeldLike(c, ValuesSword, given);
        if (sword.IsNone)
        {
            await c.Verdict();
            return;
        }
        var g0 = Items.GearOf(sword);
        c.OnEnd(() =>
        {
            if (Items.ValuesOf(sword) is null) return;
            Items.SetPower(sword, 1);
            _ = Items.SetDurabilityMax(sword, null);
        });
        c.Instruct($"Open the backpack and select the new sword ({ValuesSword}); note its damage number and its LEVEL line. Press continue.");
        await c.Verdict();
        var p = Items.SetPower(sword, 2);
        var lv = await Items.SetLevel(sword, 40, new GearWriteOptions { AnyLevel = true });
        var dm = await Items.SetDurabilityMax(sword, 2000);
        c.Expect(p.Ok && lv.Ok && dm.Ok, "power x2, level 40, max durability 2000", $"{p}; {lv}; {dm}");
        c.Instruct("Select another item and the sword again. Does its damage number read about twice what it was (scaled by Kiln on this game), " +
                   $"its LEVEL line 40 (was {g0?.Level}), and its durability bar full at a larger number if the game shows one? Say what each shows.");
        await c.Verdict();
        foreach (var (item, count) in given) await Items.Take(c.Player, item, count);
    }

    [ClientCheck("equip-p7.reload", "Per-item values come back after the world is saved and hosted again", Area = GearArea,
        Since = "0.2.0-dev.29.19", Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2973,
        Confirm = "This gives you a sword that stays in your backpack across a restart of the world. Use a disposable character in a copied world.")]
    public async Task ValuesReload(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, "items.gear.power")) return;
        var given = new List<(ItemId, int)>();
        var sword = await GiveHeldLike(c, ValuesSword, given);
        if (sword.IsNone)
        {
            await c.Verdict();
            return;
        }
        var p = Items.SetPower(sword, 2);
        var lv = await Items.SetLevel(sword, 40, new GearWriteOptions { AnyLevel = true });
        var dm = await Items.SetDurabilityMax(sword, 2000);
        c.Expect(p.Kept && lv.Kept && dm.Kept, "the three values are kept with the sword", $"{p}; {lv}; {dm}");
        c.Instruct($"Keep the sword ({ValuesSword}) in your backpack. Quit to the main menu (the world saves), host the same world again, " +
                   "wait ten seconds, then select the sword: does its LEVEL line read 40 again and its damage number about twice the plain sword's? " +
                   "Then drop the sword (it was given for this check).");
        await c.Verdict();
    }
}
