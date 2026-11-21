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
        int Hits(int ph) { lock (hits) return hits.Count(h => h.Phase == ph); }
        c.Instruct($"Equip the new sword ({ValuesSword}) and hit one creature (a training dummy or a weak enemy) three times, " +
                   "watching how much each hit takes off. The check goes on by itself after the third hit.");
        await c.Until(() => Hits(1) >= 3, TimeSpan.FromMinutes(5), "three hits with the sword at power x1");
        await c.Delay(TimeSpan.FromSeconds(2));  // a swing's late damage still counts as round one
        var p = Items.SetPower(sword, 3);
        c.Expect(p.Ok, "the sword's power x3", p.ToString());
        phase = 2;
        c.Instruct("The sword now has power x3. Hit the same creature three more times with the same sword. The check goes on by " +
                   "itself after the third hit.");
        await c.Until(() => Hits(2) >= 3, TimeSpan.FromMinutes(5), "three hits with the sword at power x3");
        await c.Delay(TimeSpan.FromSeconds(2));
        List<(int Phase, EntityId Target, int Amount)> seen;
        lock (hits) seen = [.. hits];
        double Avg(int ph) => seen.Where(h => h.Phase == ph).Select(h => (double)h.Amount).DefaultIfEmpty(0).Average();
        var (a1, a3) = (Avg(1), Avg(2));
        c.Expect(a1 > 0 && a3 > 0, "hits seen in both phases", $"{seen.Count(h => h.Phase == 1)} then {seen.Count(h => h.Phase == 2)} hits");
        c.Expect(a1 > 0 && a3 >= 2 * a1, "the damage grew about threefold", $"average {a1:0.#} at x1, {a3:0.#} at x3");
        c.Instruct("Pass if the damage numbers you saw (or the creature's health bar) dropped clearly faster in the second round " +
                   "than in the first; otherwise Fail with a note saying what you saw.");
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
        int Taken(int ph) { lock (taken) return taken.Count(h => h.Phase == ph); }
        c.Instruct($"Wear the new chest piece ({ValuesChest}) and let one weak creature hit you three times, watching how much health " +
                   "each hit takes. Also note the character screen's armour value. The check goes on by itself after the third hit.");
        await c.Until(() => Taken(1) >= 3, TimeSpan.FromMinutes(5), "three hits taken at armour x1");
        await c.Delay(TimeSpan.FromSeconds(2));
        var a = Items.SetArmor(chest, 10);
        c.Expect(a.Ok, "the chest piece's armour x10", a.ToString());
        phase = 2;
        c.Instruct("The chest piece now has armour x10. Let the same creature hit you three more times. The check goes on by itself " +
                   "after the third hit.");
        await c.Until(() => Taken(2) >= 3, TimeSpan.FromMinutes(5), "three hits taken at armour x10");
        await c.Delay(TimeSpan.FromSeconds(2));
        List<(int Phase, int Amount)> seen;
        lock (taken) seen = [.. taken];
        double Avg(int ph) => seen.Where(h => h.Phase == ph).Select(h => (double)h.Amount).DefaultIfEmpty(0).Average();
        c.Expect(Avg(1) > 0 && Avg(2) > 0 && Avg(2) < Avg(1), "the damage taken went down", $"average {Avg(1):0.#} at x1, {Avg(2):0.#} at x10");
        c.Instruct("Pass if you lost clearly less health per hit in the second round; otherwise Fail. Either way, in the note say " +
                   "what the character screen's armour value shows now compared with before.");
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
        var watch = WatchDetail(c, Items.Lookup(ValuesSword)?.Id ?? default);
        c.OnEnd(() =>
        {
            if (Items.ValuesOf(sword) is null) return;
            Items.SetPower(sword, 1);
            _ = Items.SetDurabilityMax(sword, null);
        });
        c.Instruct($"Open the backpack and select the new sword ({ValuesSword}); note its damage number and its LEVEL line. " +
                   Then(watch, "its detail view has shown for ten seconds", 60));
        await UntilViews(c, watch, 1, "the sword's detail view shown", 50, linger: 10);
        var p = Items.SetPower(sword, 2);
        var lv = await Items.SetLevel(sword, 40, new GearWriteOptions { AnyLevel = true });
        var dm = await Items.SetDurabilityMax(sword, 2000);
        c.Expect(p.Ok && lv.Ok && dm.Ok, "power x2, level 40, max durability 2000", $"{p}; {lv}; {dm}");
        c.Instruct("The sword has changed. Select another item and the sword again. Pass only if its damage number reads about twice " +
                   $"what it was (scaled by Kiln on this game) and its LEVEL line reads 40 (was {g0?.Level}); in the note say what " +
                   "each shows and whether the durability bar is full at a larger number (if the game shows one).");
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
