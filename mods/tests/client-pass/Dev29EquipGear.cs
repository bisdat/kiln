// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Equipment wave P6 (0.2.0-dev.29.19): typed gear values (Content.Gear, the data packs' "gear" section). Orders 2960-2969.
//
// - equip-p6.gear-tooltip: in a hosted world (the hosting game is the server, so its registry holds the values), a gear power
//   and a gear armour written through Content.Gear show in the game's own item tooltip as the numbers written: Kiln solves
//   them with the game's formulas (damage: int(int(10 * 0.15 * level + 10) * dmgMod * speedMod); armour:
//   round((15 * level + 23) * share)), checked offline at every level, never yet against the game's drawn tooltip.
//   Both are taken back at the end (GearStats.Reset), and the items given are taken back.
public sealed partial class ClientPassMod
{
    private const string GearArea = "Equipment";
    private const string GearSword = "Weapon_T1_1H_Sword_Craft";      // Common, level 5, damage set
    private const string GearChest = "Armor_T2_Warrior_Tank_Chest";   // Rare, level 10, physical and magical armour

    [ClientCheck("equip-p6.gear-tooltip", "Gear values show in the game's tooltip as written", Area = GearArea, Since = "0.2.0-dev.29.19",
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2960,
        Confirm = "This gives you a sword and a chest piece for a moment and takes them back. Use a disposable character in a copied world.")]
    public async Task GearTooltip(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, Content.GearCapability)) return;
        var maybeSword = Content.Gear(GearSword);
        var maybeChest = Content.Gear(GearChest);
        if (!c.Expect(maybeSword is { HasDamage: true } && maybeChest is { HasArmor: true }, "the sword and the chest piece are in this game",
                $"{GearSword}: {(maybeSword == null ? "missing" : $"damage {maybeSword.HasDamage}")}, {GearChest}: {(maybeChest == null ? "missing" : $"armour {maybeChest.HasArmor}")}"))
        {
            await c.Verdict();
            return;
        }
        var sword = maybeSword!;
        var chest = maybeChest!;
        var p = c.Player;
        var given = new List<(ItemId Item, int Count)>();
        c.OnEnd(() =>
        {
            sword.Reset();
            chest.Reset();
        });
        var before = (sword.Power, chest.Armor(GearArmorKind.Physical));
        sword.Power = 77;
        chest.SetArmor(GearArmorKind.Physical, 99);
        c.Expect(sword.Power == 77 && chest.Armor(GearArmorKind.Physical) == 99, "the values read back as written",
            $"power {sword.Power} (was {before.Item1}) at level {sword.Level}, physical armour {chest.Armor(GearArmorKind.Physical)} (was {before.Item2}) at level {chest.Level}");
        foreach (var item in new[] { GearSword, GearChest })
        {
            var g = await Items.Give(p, item, 1);
            if (g.Succeeded) given.Add((g.Item, 1));
            c.Expect(g.Succeeded, $"{item} given", g.ToString());
        }
        var swordId = Items.Lookup(GearSword)?.Id ?? default;
        var chestId = Items.Lookup(GearChest)?.Id ?? default;
        using (var seenSword = new DetailWatch(swordId))
        using (var seenChest = new DetailWatch(chestId))
        {
            var watched = seenSword.Ok && seenChest.Ok;
            c.Instruct($"Open your inventory and look at the new sword ({GearSword}): its tooltip should read damage 77 (the game's own is {before.Item1}). " +
                       $"Then look at the new chest piece ({GearChest}): its physical armour should read 99 (the game's own is {before.Item2}). " +
                       "Remember what each shows. " +
                       (watched ? "The check takes the values back by itself ten seconds after you have looked at both."
                                : "The check takes the values back by itself in 90 seconds."));
            if (watched)
                await c.Until(() => seenSword.Views > 0 && seenChest.Views > 0, TimeSpan.FromMinutes(5), "both items' detail views shown");
            else
                await c.Delay(TimeSpan.FromSeconds(80));
            await c.Delay(TimeSpan.FromSeconds(10));
        }
        sword.Reset();
        chest.Reset();
        c.Instruct($"The values are taken back. Select another item, then the same two items again. Pass only if both held: (1) before, " +
                   $"the sword read damage 77 and the chest piece physical armour 99; (2) now they read the game's own numbers again " +
                   $"(damage {before.Item1}, physical armour {before.Item2}). Otherwise Fail with a note naming which part and what you saw.");
        await c.Verdict();
        foreach (var (item, count) in given) await Items.Take(p, item, count);
    }
}
