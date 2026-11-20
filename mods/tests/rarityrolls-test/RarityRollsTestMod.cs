// SPDX-License-Identifier: MIT
// Lab test of rarity rolls at creation (RarityRollBlock, kiln.items v8 rarity_rolls). Lines "PASS/FAIL ..." and
// "rarityrolls-test done: N PASS, M FAIL" in kiln.log.
using Kiln;

namespace RarityRollsTest;

public sealed class RarityRollsTestMod : Mod
{
    private const string Chest = "Armor_T2_Warrior_Tank_Chest";   // five perk slots filled, type rarity Rare, one level
    private const string Plain = "Weapon_T1_1H_Sword_Craft";      // no rule names it
    private const string Twigs = "Material_T1_Twigs";
    private static readonly Guid ChestRecipe = Guid.Parse("5c1d7e10-8a2b-4f3c-9d4e-6a7b8c9d0e15");
    private const int Rolls = 200;

    private int _pass, _fail;
    private bool _stop, _ran;
    private EntityId _box;

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
        if (!Content.Available || !Items.Available || !World.Available)
        {
            Log.Warn("rarityrolls-test: needs kiln.content, kiln.items, kiln.world");
            return;
        }
        Content.Recipes.Register(new RecipeDefinition { Guid = ChestRecipe, Name = "Kiln_RR_Chest_Recipe", Station = "hand", Inputs = [(Twigs, 1)], Outputs = [(Chest, 1)] });
        // crafts and plain gives: Common 1, Epic 1, Legendary 2 (Uncommon and Rare never)
        Blocks.Define("chest-rolls", new RarityRollBlock
        {
            Items = [Chest], Sources = ItemSource.Craft | ItemSource.Give,
            Weights = new Dictionary<Rarity, double> { [Rarity.Common] = 1, [Rarity.Epic] = 1, [Rarity.Legendary] = 2 },
        });
        Content.OnReady(ev =>
        {
            if (_ran) return;
            _ran = true;
            _ = Run();
        });
    }

    // the rarity of the item entity a give made (PerkContainerNew.availablePerks - 1; -1: no container), then take it back
    private async Task<int> Made(Task<ItemsResult> give, string item)
    {
        var r = await give;
        var ch = r.Changes.FirstOrDefault(c => !c.ItemEntity.IsNone);
        if (r.Status != ItemsStatus.Done || ch.ItemEntity.IsNone)
        {
            Check(false, $"{item}: no item entity: {r}");
            return -2;
        }
        await Steps(1);
        var perks = World.ReadBytes(ch.ItemEntity, "PerkContainerNew");
        var rarity = perks is { Length: >= 2 } ? perks[0] - 1 : -1;
        await Items.Take(_box, ch.Item, 1, ch.Slot, exactSlot: true);
        return rarity;
    }

    private async Task Run()
    {
        try
        {
            for (var i = 0; i < 25 * 120 && !_stop && Items.Status() is not { Transactions: true, Catalog: true, Read: true }; i++) await Steps(1);
            await Steps(25 * 3);  // the rules resolve on Kiln.Blocks' tick, every 48 steps
            foreach (var e in Items.FindInventories(InventoryKinds.Containers | InventoryKinds.NonEmpty))
                if (Items.Read(e) is { Initialized: true } c && c.FreeSlots() >= 2) { _box = e; break; }
            if (_box.IsNone)
            {
                Check(false, "a container with free slots");
                return;
            }
            var recipe = Content.Recipes.Get("Kiln_RR_Chest_Recipe");
            var counts = new int[7];
            for (var i = 0; i < Rolls && !_stop; i++)
            {
                var made = i % 2 == 0 && recipe != null
                    ? await Made(Items.GiveRecipeResult(_box, recipe.Id), "craft")
                    : await Made(Items.Give(_box, Chest), "give");
                counts[made + 2]++;
            }
            var (common, uncommon, rare, epic, legendary) = (counts[2], counts[3], counts[4], counts[5], counts[6]);
            Log.Info($"rarityrolls-test: {Rolls} made: Common {common}, Uncommon {uncommon}, Rare {rare}, Epic {epic}, Legendary {legendary}, other {counts[0] + counts[1]}");
            Check(uncommon == 0 && rare == 0 && counts[0] + counts[1] == 0, "only the rarities with a weight come up (the type's own Rare never)");
            // expected 50 / 50 / 100 of 200: each within +-25 (well over 3 sigma)
            Check(Math.Abs(common - 50) <= 25 && Math.Abs(epic - 50) <= 25 && Math.Abs(legendary - 100) <= 25, "the spread follows the weights 1:1:2");
            var asked = await Made(Items.Give(_box, Chest, 1, new Gear(Rarity: ItemRarity.Uncommon)), "give with a rarity");
            Check(asked == 1, $"a give with Gear.Rarity Uncommon is never rolled: {asked}");
            var plain = await Made(Items.Give(_box, Plain), "plain");
            Check(plain == 0, $"an item type no rule names keeps the game's rarity (Common): {plain}");
        }
        catch (Exception e)
        {
            Check(false, $"the test threw: {e}");
        }
        Log.Info($"rarityrolls-test done: {_pass} PASS, {_fail} FAIL");
    }
}
