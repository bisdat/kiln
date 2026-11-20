// SPDX-License-Identifier: MIT
// Lab test of rarity upgrades (Items.UpgradeRarity, kiln.items v8 gear_upgrade), player-free: the "player" is a chest (the
// call needs someone to run for; payers are containers). Lines "PASS/FAIL ..." and "upgrades-test done: N PASS, M FAIL"
// in kiln.log.
using Kiln;

namespace UpgradesTest;

public sealed class UpgradesTestMod : Mod
{
    private const string Armour = "Armor_T2_Warrior_Tank_Chest";  // five perk slots filled, type rarity Rare
    private const string Twigs = "Material_T1_Twigs";             // a plain item: the cost

    private int _pass, _fail;
    private bool _stop, _ran;

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

    public override void OnUnload() => _stop = true;

    public override void OnLoad()
    {
        if (!Items.Available)
        {
            Log.Warn("upgrades-test: needs kiln.items");
            return;
        }
        Blocks.Define("lab-upgrades", new RarityUpgradeBlock
        {
            Items = [Armour],
            Steps =
            {
                [Rarity.Epic] = new UpgradeStep { Cost = { [Twigs] = 10 } },
                [Rarity.Legendary] = new UpgradeStep { Cost = { [Twigs] = 5 }, Chance = 0, OnFail = UpgradeFail.KeepItemLoseCost },
            },
        });
        Context.Tick.Pre += (in TickInfo t) =>
        {
            if (_ran || t.Step % 25 != 0 || !Lifecycle.IsServerRunning || !Items.UpgradeAvailable) return;
            _ran = true;
            _ = Run();
        };
    }

    private static int Count(EntityId holder, ItemId item) => Items.Read(holder)?.Slots.Where(s => s.Item == item).Sum(s => s.Count) ?? -1;

    private async Task<ItemInstanceRef> GiveArmour(EntityId box)
    {
        var r = await Items.Give(box, Items.Lookup(Armour)!.Id, 1, new Gear(Rarity: ItemRarity.Rare));
        return r.Changes.FirstOrDefault(c => !c.Ref.IsNone).Ref;
    }

    private async Task Run()
    {
        try
        {
            await Steps(25 * 3);
            var boxes = new List<EntityId>();
            foreach (var e in Items.FindInventories(InventoryKinds.Containers))
                if (Items.Read(e) is { Initialized: true } c && c.FreeSlots() >= 4) boxes.Add(e);
            if (boxes.Count < 2)
            {
                Check(false, $"two containers with free slots (found {boxes.Count})");
                return;
            }
            var (a, b) = (boxes[0], boxes[1]);
            var twigs = Items.Lookup(Twigs)!.Id;
            Log.Info($"upgrades-test: chests {a} and {b}; twigs {Count(a, twigs)} / {Count(b, twigs)}");
            var armour = await GiveArmour(a);
            Check(!armour.IsNone && Items.GearOf(armour)?.Rarity == ItemRarity.Rare, $"a Rare armour piece in chest {a}: {Items.GearOf(armour)}");

            // 1: the cost from the same chest, the write in the same step
            var before = Count(a, twigs);
            await Items.Give(a, twigs, 10);
            var r1 = await Items.UpgradeRarity(a, armour, "lab-upgrades", new UpgradeOptions { PayFrom = a });
            Check(r1.Ok && r1.After?.Rarity == ItemRarity.Epic && Count(a, twigs) == before, $"Rare -> Epic paid by its chest: {r1}; twigs {Count(a, twigs)} (was {before})");
            await Steps(2);
            var g = Items.GearOf(armour);
            Check(g is { Rarity: ItemRarity.Epic, ShownRarity: ItemRarity.Epic, Enhancements: 4 }, $"the game shows Epic a step later, the same item entity: {g}");

            // 2: too little: nothing taken, nothing written (Legendary costs 5; give 3)
            var b0 = Count(b, twigs);
            await Items.Give(b, twigs, 3);
            var r2 = await Items.UpgradeRarity(a, armour, "lab-upgrades", new UpgradeOptions { PayFrom = b });
            Check(r2.Status == UpgradeStatus.CantPay && Count(b, twigs) == b0 + 3 && Items.GearOf(armour)?.Rarity == ItemRarity.Epic,
                $"too little in the other chest: {r2}; twigs there {Count(b, twigs)}");

            // 3: a failed roll (chance 0) keeps the item and spends the cost, from the other chest
            await Items.Give(b, twigs, 2);
            var r3 = await Items.UpgradeRarity(a, armour, "lab-upgrades", new UpgradeOptions { PayFrom = b });
            Check(r3.Status == UpgradeStatus.Failed && Count(b, twigs) == b0 && Items.GearOf(armour)?.Rarity == ItemRarity.Epic,
                $"a failed roll keeps the item, the cost spent: {r3}; twigs there {Count(b, twigs)}");

            // 4: two upgrades of one item in one step: one pays and writes, the other is refused and pays nothing
            var second = await GiveArmour(a);
            var before4 = Count(a, twigs);
            await Items.Give(a, twigs, 20);
            var t1 = Items.UpgradeRarity(a, second, "lab-upgrades", new UpgradeOptions { PayFrom = a });
            var t2 = Items.UpgradeRarity(a, second, "lab-upgrades", new UpgradeOptions { PayFrom = a });
            var (u1, u2) = (await t1, await t2);
            Check(u1.Ok && u2.Status == UpgradeStatus.Refused && Count(a, twigs) == before4 + 10 && Items.GearOf(second)?.Rarity == ItemRarity.Epic,
                $"two upgrades in one step: {u1.Status} and {u2.Status} ({u2.Reason}); twigs {Count(a, twigs)} (was {before4} + 20)");

            // 5: a dry run pays and changes nothing
            await Items.Give(a, twigs, 5);
            var before5 = Count(a, twigs);
            var d = await Items.UpgradeRarity(a, second, "lab-upgrades", new UpgradeOptions { PayFrom = a, DryRun = true });
            Check(d.Ok && d.DryRun && Count(a, twigs) == before5 && Items.GearOf(second)?.Rarity == ItemRarity.Epic, $"a dry run: {d}");
            Log.Info($"upgrades-test: gear writes {Items.GearWriteAvailable}, upgrades {Items.UpgradeAvailable}");
        }
        catch (Exception e)
        {
            Check(false, $"the test threw: {e}");
        }
        finally
        {
            Log.Info($"upgrades-test done: {_pass} PASS, {_fail} FAIL");
        }
    }
}
