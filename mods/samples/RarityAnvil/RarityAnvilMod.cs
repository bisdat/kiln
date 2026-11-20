// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.GameIds;

namespace RarityAnvil;

/// <summary>
/// Rarity upgrades at a station. The rule says which items, up to which rarity and what each step costs; the anvil runs it on the
/// item in the player's hand, the forge in a window that lists the player's gear. An upgraded item gets its next enhancement slot,
/// which the player unlocks at a workbench as usual. blocks/twins.json says the same in data.
/// </summary>
public sealed class RarityAnvilMod : Mod
{
    public override void OnLoad()
    {
        // The rule: weapons, armour, rings and this mod's own sword, one rarity per step. Later steps cost more and can fail;
        // a failed roll keeps the item and spends the cost (the default). The cost grows a little with the item's level.
        var upgrades = Blocks.Define("standard-upgrades", new RarityUpgradeBlock
        {
            Items = ["Weapon_*", "Armor_*", "Ring_*", "anvil-blade"],
            Steps =
            {
                [Rarity.Uncommon] = new UpgradeStep { Cost = { [GameItems.Material_T1_Metal_Scraps] = 10 } },
                [Rarity.Rare] = new UpgradeStep { Cost = { [GameItems.Material_T1_Metal_Sheets] = 5 }, Chance = 0.9 },
                [Rarity.Epic] = new UpgradeStep { Cost = { [GameItems.Material_T3_Bronze_Bar] = 5 }, Chance = 0.75, LevelScale = 0.05 },
                [Rarity.Legendary] = new UpgradeStep { Cost = { [GameItems.Material_T4_Iron_Bar] = 8, [GameItems.Material_T1_KryptCrystal] = 1 }, Chance = 0.5, LevelScale = 0.05 },
            },
        });

        // The anvil: hold a weapon, a shield or a tool and use it. Works for every player; the outcome is a toast on a Kiln
        // player's game and a chat line for others.
        Blocks.Define("rarity-anvil", new PropBlock
        {
            Name = "Rarity Anvil", Description = "Hold the item you want to improve and strike.",
            Visual = new VisualBlock { Like = GameItems.Prop_Decoration_Comfort1_T0_Table_Medium },
            Recipe = new RecipeBlock { Station = StationKind.Workbench.Workshop, Needs = { [GameItems.Material_T1_Metal_Sheets] = 10, [GameItems.Material_T1_Wood_Logs] = 4 } },
            OnInteract = new InteractBlock { Prompt = "Upgrade held item", Upgrade = new UpgradeAction { Rule = upgrades.ToString() } },
        });

        // The forge: a window lists every piece of gear you carry, armour and rings too, with the next step's cost and chance.
        // A Kiln player's game draws it; a player without Kiln gets the held item, as at the anvil.
        Blocks.Define("rarity-forge", new PropBlock
        {
            Name = "Rarity Forge", Description = "Lay out your gear and choose what to improve.",
            Visual = new VisualBlock { Like = GameItems.Prop_Decoration_Comfort1_T0_Table_Medium },
            Recipe = new RecipeBlock { Station = StationKind.Workbench.Workshop, Needs = { [GameItems.Material_T1_Metal_Sheets] = 20 } },
            OnInteract = new InteractBlock { Prompt = "Upgrade gear", Upgrade = new UpgradeAction { Rule = "standard-upgrades", Target = UpgradeTarget.Choose } },
        });

        // Something to upgrade: a sword made at the workbench, whose rarity is rolled when it is crafted.
        Blocks.Define("anvil-blade", new WeaponBlock
        {
            Name = "Anvil Blade", Class = WeaponClass.Sword,
            Stats = new WeaponStats { Level = 5, Power = 25 },
            Recipe = new RecipeBlock { Station = StationKind.Workbench.Workshop, Needs = { [GameItems.Material_T1_Metal_Scraps] = 3 } },
        });
        Blocks.Define("anvil-blade-rolls", new RarityRollBlock
        {
            Items = ["anvil-blade"],
            Weights = new Dictionary<Rarity, double> { [Rarity.Common] = 50, [Rarity.Uncommon] = 30, [Rarity.Rare] = 15, [Rarity.Epic] = 4, [Rarity.Legendary] = 1 },
        });

        // Every upgrade that happened, as a line in the mod's log (the station tells the player itself).
        Events.On<GearUpgradedEvent>(e => Log.Info($"{e.Player}: {e.Result}"));
    }
}
