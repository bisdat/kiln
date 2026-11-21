// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Equipment wave (0.2.0-dev.29.19): what only a player in the real game can show of rarity changes, upgrades and the upgrade
// station (packages P1 gear writes, P2 reads and events, P3 upgrades, P4 the station, P5 rarity rolls). Orders 7010-7019.
//  - equip.c1  the anvil upgrades the held item: the prompt, the hand on it, the toast, and the item's new rarity at once
//  - equip.c2  the forge's window on a Kiln player's game: the list, the button, the refresh, the range
//  - equip.c3  the workbench unlocks the slot an upgrade added, for the game's own cost; Kiln sees it (GearEnhancedEvent)
//  - equip.c4  durability after an upgrade: the share kept, the new max shown
//  - equip.c10 crafted rarity rolls: a recipe's item comes in rolled rarities, its tooltip still shows the type's
//  - equip.c11 a rarity written from code (Items.SetRarity) up and down, seen by the holder without re-joining
// The tooltip lines and values are equip.c5 to equip.c9 (EquipTooltips.cs); typed gear values equip-p6.gear-tooltip.
// Each uses the sample mods/samples/RarityAnvil (mod rarity-anvil) and fails closed without it.
public sealed partial class ClientPassMod
{
    private const string EquipArea = "Equipment";
    private const string EquipSince = "0.2.0-dev.29.19";
    private const string AnvilMod = "rarity-anvil";
    private const string AnvilBlade = "rarity-anvil.anvil-blade";         // the sample's sword: five enhancement slots (class Sword)
    private const string AnvilProp = "rarity-anvil.rarity-anvil.prop";    // held item
    private const string ForgeProp = "rarity-anvil.rarity-forge.prop";    // the window
    private const string FivePerkChest = "Armor_T2_Warrior_Tank_Chest";   // five perk slots, type rarity Rare
    private const string EquipConfirm = "This gives you items and places a station for the check, and takes the station away at the end. Use a disposable character in a copied world.";

    /// <summary>Gives the player one piece of gear at a rarity and returns it (none when the give failed).</summary>
    private static async Task<ItemInstanceRef> GiveGear(CheckContext c, string item, ItemRarity rarity)
    {
        if (Items.Lookup(item) is not { } info)
        {
            c.Expect(false, $"{item} is an item of this game");
            return ItemInstanceRef.None;
        }
        var r = await Items.Give(c.Player, info.Id, 1, new Gear(Rarity: rarity));
        var made = r.Changes.FirstOrDefault(x => !x.Ref.IsNone).Ref;
        c.Expect(r.Succeeded && !made.IsNone, $"gave a {rarity} {item}", r.ToString());
        return made;
    }

    private static async Task<bool> NeedEquip(CheckContext c, params string[] capabilities)
    {
        if (!await Need(c, AnvilMod) || !await NeedPlayer(c)) return false;
        var off = capabilities.Where(x => !Capabilities.Is(x)).ToArray();
        if (off.Length == 0) return true;
        c.Expect(false, $"{string.Join(", ", off)} on", Capabilities.Why(off[0]));
        await c.Verdict();
        return false;
    }

    [ClientCheck("equip.c1", "An upgrade station upgrades the held item, and the game shows it at once", Area = EquipArea, Since = EquipSince,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7010, Confirm = EquipConfirm)]
    public async Task EquipStationHeld(CheckContext c)
    {
        if (!await NeedEquip(c, "items.gear.upgrade", UpgradeStation.Capability)) return;
        if (Ahead(c, 2.5) is not { } at) { c.Expect(false, "the player's place is known"); await c.Verdict(); return; }
        var upgraded = new List<GearUpgradedEvent>();
        var sub = Events.On<GearUpgradedEvent>(e => { if (e.Player == c.Player) upgraded.Add(e); });
        c.OnEnd(() => sub.Dispose());
        var blade = await GiveGear(c, AnvilBlade, ItemRarity.Common);
        await GiveGame(c, "Material_T1_Metal_Scraps", 30);
        await GiveGame(c, "Material_T1_Metal_Sheets", 15);
        var anvil = await SpawnHere(c, AnvilProp, at);
        if (blade.IsNone || anvil.IsNone) { await c.Verdict(); return; }
        c.Instruct("A Rarity Anvil stands in front of you and you have a Common Anvil Blade, 30 metal scraps and 15 metal sheets. " +
                   "Put the Anvil Blade on your action bar and hold it, look at the anvil: does the prompt read \"Upgrade held item\"? " +
                   "Remember the answer and use it (E). The check goes on by itself after the upgrade.");
        c.Expect(await c.Until(() => upgraded.Count > 0, TimeSpan.FromMinutes(5), "an upgrade ran"), "the anvil ran an upgrade for you",
            upgraded.Count > 0 ? upgraded[0].Result.ToString() : "no GearUpgradedEvent");
        c.Expect(Items.GearOf(blade)?.Rarity == ItemRarity.Uncommon, "the blade is Uncommon on the server", Items.GearOf(blade)?.ToString());
        c.Instruct("Remember: did your character put a hand on the anvil, and did a toast say \"Anvil Blade is now Uncommon: unlock its new " +
                   "enhancement at a workbench. Paid 10 x Metal Scraps.\"? Look at the blade's tooltip: does it show the Uncommon colour " +
                   "and two enhancement rows (one locked), without leaving and re-joining? Then use the anvil again with the blade in " +
                   "hand (Uncommon to Rare costs 5 sheets, 90 % chance). The check goes on by itself after that second upgrade.");
        c.Expect(await c.Until(() => upgraded.Count >= 2, TimeSpan.FromMinutes(5), "a second upgrade ran"), "a second upgrade ran",
            string.Join("; ", upgraded.Select(u => u.Result.ToString())));
        c.Instruct("Now hold nothing and use the anvil once more. Pass only if all held: (1) the prompt read \"Upgrade held item\"; " +
                   "(2) after the first use, a hand on the anvil, the \"is now Uncommon ... Paid 10 x Metal Scraps.\" toast, and the " +
                   "tooltip in the Uncommon colour with two enhancement rows (one locked); (3) the second toast read either " +
                   "\"... is now Rare ...\" or \"The upgrade ... failed (90% chance) ...\"; (4) the empty hand got \"Hold the item you " +
                   "want to upgrade in your hand, then use it again.\". Otherwise Fail with a note naming which part.");
        await c.Verdict();
    }

    [ClientCheck("equip.c2", "The upgrade window lists your gear and upgrades the one you pick", Area = EquipArea, Since = EquipSince,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7011, Confirm = EquipConfirm)]
    public async Task EquipStationWindow(CheckContext c)
    {
        if (!await NeedEquip(c, "items.gear.upgrade", UpgradeStation.Capability)) return;
        if (Ahead(c, 2.5) is not { } at) { c.Expect(false, "the player's place is known"); await c.Verdict(); return; }
        var upgraded = new List<GearUpgradedEvent>();
        var sub = Events.On<GearUpgradedEvent>(e => { if (e.Player == c.Player) upgraded.Add(e); });
        c.OnEnd(() => sub.Dispose());
        var chest = await GiveGear(c, FivePerkChest, ItemRarity.Rare);
        await GiveGear(c, AnvilBlade, ItemRarity.Epic);
        await GiveGame(c, "Material_T3_Bronze_Bar", 30);
        var forge = await SpawnHere(c, ForgeProp, at);
        if (chest.IsNone || forge.IsNone) { await c.Verdict(); return; }
        c.Instruct("A Rarity Forge stands in front of you; you have a Rare Warrior chest piece, an Epic Anvil Blade and 30 bronze bars. " +
                   "Use the forge (E, \"Upgrade gear\"). A Kiln window \"Rarity upgrade\" should open and list your gear: the chest piece " +
                   "Rare -> Epic for about 8 bronze bars (the price grows with the item's level) and a 75% chance, with an Upgrade button; " +
                   "the blade Epic -> Legendary with \"Not enough to upgrade ...\" instead of a button. Remember whether that is what " +
                   "you see, then press Upgrade on the chest piece. The check goes on by itself after that upgrade.");
        c.Expect(await c.Until(() => upgraded.Any(u => u.Item.Entity.Id == chest.Entity.Id), TimeSpan.FromMinutes(5), "the pick ran"),
            "the window's pick ran the upgrade of the chest piece", string.Join("; ", upgraded.Select(u => u.Result.ToString())));
        c.Instruct("Look at the toast and the window. Then walk more than 6 m away from the forge and press an Upgrade button in the " +
                   "window; close the window with Close and check the chest piece's tooltip in the inventory. Pass only if all held: " +
                   "(1) the window listed the gear as described; (2) a toast said what the upgrade did (Epic, or failed at 75 %) and the " +
                   "window came back with the chest piece's next step (or the same one after a failure); (3) from more than 6 m a toast " +
                   "said \"You are too far from the station: go back and use it again.\", with nothing paid; (4) the chest piece's " +
                   "rarity colour and enhancement rows match the last toast, without re-joining. Otherwise Fail with a note naming which part.");
        await c.Verdict();
    }

    [ClientCheck("equip.c3", "The workbench unlocks the slot an upgrade added, for the game's own cost", Area = EquipArea, Since = EquipSince,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7012, Confirm = EquipConfirm)]
    public async Task EquipUnlockAfterUpgrade(CheckContext c)
    {
        if (!await NeedEquip(c, "items.gear.upgrade", "items.gear.read")) return;
        var enhanced = new List<GearEnhancedEvent>();
        var sub = Events.On<GearEnhancedEvent>(e => enhanced.Add(e));
        c.OnEnd(() => sub.Dispose());
        var blade = await GiveGear(c, AnvilBlade, ItemRarity.Common);
        if (blade.IsNone) { await c.Verdict(); return; }
        var rule = new RarityUpgradeRule { Steps = { [ItemRarity.Uncommon] = new UpgradeStep() } };
        var up = await Items.UpgradeRarity(c.Player, blade, rule);
        c.Expect(up.Ok, "the blade was upgraded to Uncommon for free", up.ToString());
        var before = Items.GearOf(blade);
        c.Expect(before is { Enhancements: 2, Unlocked: 0 }, "the blade has two enhancement slots, none unlocked", before?.ToString());
        c.Instruct("You have an Uncommon Anvil Blade with two enhancement slots, both locked. Go to a workbench and unlock both of " +
                   "its enhancements (have the runes it asks for). Remember whether the workbench offers the second slot and whether its " +
                   "cost looks like the game's for an Uncommon weapon. The check goes on by itself when both are unlocked.");
        c.Expect(await c.Until(() => Items.GearOf(blade) is { Unlocked: 2 }, TimeSpan.FromMinutes(10), "both unlocked"),
            "the server has both enhancements unlocked", Items.GearOf(blade)?.ToString());
        c.Expect(enhanced.Any(e => e.Item.Entity.Id == blade.Entity.Id && e.Source == GearChangeSource.Game), "Kiln saw the game's unlock (GearEnhancedEvent, source Game)",
            string.Join("; ", enhanced.Select(e => e.ToString())));
        c.Instruct($"Both are unlocked. Pass only if all held: (1) the workbench offered the second slot, at a cost like the game's for " +
                   $"an Uncommon weapon; (2) the blade's tooltip now reads LEVEL {(before?.Level ?? 5) + 2}, with both perks listed as active; " +
                   "(3) its damage is higher than before the unlocks. Otherwise Fail with a note naming which part.");
        await c.Verdict();
    }

    [ClientCheck("equip.c4", "Durability after an upgrade keeps its share and shows the new maximum", Area = EquipArea, Since = EquipSince,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7013, Confirm = EquipConfirm)]
    public async Task EquipDurabilityAfterUpgrade(CheckContext c)
    {
        if (!await NeedEquip(c, "items.gear.upgrade", "items.gear.durability")) return;
        var blade = await GiveGear(c, AnvilBlade, ItemRarity.Common);
        if (blade.IsNone) { await c.Verdict(); return; }
        var watch = WatchDetail(c, Items.Lookup(AnvilBlade)?.Id ?? default);
        c.Instruct("Hold the new Common Anvil Blade and hit something (a tree, a rock) until its durability bar is visibly below full. " +
                   "The check goes on by itself when the blade has lost a twentieth of its durability.");
        await c.Until(() => Items.GearOf(blade) is { Durability: { } d0, DurabilityMax: { } m0 } && m0 > 0 && d0 <= m0 * 0.95,
            TimeSpan.FromMinutes(5), "the blade worn below 95 %");
        var views = watch.Views;
        c.Instruct("Stop hitting. Open the inventory and select the blade; note its durability bar and the durability in its detail view. " +
                   Then(watch, "its detail view has shown for ten seconds", 60));
        await UntilViews(c, watch, views + 1, "the blade's detail view shown", 50, linger: 10);
        var before = Items.GearOf(blade);
        c.Expect(before is { Durability: { } d, DurabilityMax: { } m } && d < m, "the blade is worn on the server", before?.ToString());
        var rule = new RarityUpgradeRule { Steps = { [ItemRarity.Uncommon] = new UpgradeStep() } };
        var up = await Items.UpgradeRarity(c.Player, blade, rule);
        c.Expect(up.Ok, "the blade was upgraded to Uncommon (durability keepRatio)", up.ToString());
        await c.Ticks(10);
        var after = Items.GearOf(blade);
        c.Expect(after is { DurabilityMax: { } am } && before?.DurabilityMax is { } bm && am > bm, "the max durability grew with the rarity (0.90 to 0.95 of the base)",
            $"{before?.Durability}/{before?.DurabilityMax} -> {after?.Durability}/{after?.DurabilityMax}");
        if (before is { Durability: { } bd, DurabilityMax: { } bmx } && after is { Durability: { } ad, DurabilityMax: { } amx } && bmx > 0 && amx > 0)
            c.Expect(Math.Abs((double)bd / bmx - (double)ad / amx) < 0.02, "the share is kept", $"{(double)bd / bmx:P1} -> {(double)ad / amx:P1}");
        c.Instruct($"The blade is now Uncommon. Select another item and the blade again. Pass only if its durability bar is as full as " +
                   $"before and the tooltip's durability reads about {after?.Durability} / {after?.DurabilityMax}; otherwise Fail with a note saying what it shows.");
        await c.Verdict();
    }

    [ClientCheck("equip.c10", "A crafted item comes in rolled rarities; its recipe still shows the type's", Area = EquipArea, Since = EquipSince,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7014, Confirm = EquipConfirm)]
    public async Task EquipCraftedRolls(CheckContext c)
    {
        if (!await NeedEquip(c, "items.gear.rolls")) return;
        await GiveGame(c, "Material_T1_Metal_Scraps", 30);
        var blade = Items.Lookup(AnvilBlade);
        c.Expect(blade != null, $"{AnvilBlade} is an item of this game");
        int Count() => blade == null ? 0 : Items.Read(c.Player)?.Slots.Count(s => s.Item == blade.Id) ?? 0;
        var start = Count();
        c.Instruct("Open a workbench. The Anvil Blade's recipe (3 metal scraps) should show it as Common in its tooltip: remember " +
                   "whether it does before each craft. Craft it ten times. The check goes on by itself after the tenth blade.");
        await c.Until(() => Count() - start >= 10, TimeSpan.FromMinutes(10), "ten blades crafted");
        await c.Ticks(10);
        var made = Items.Read(c.Player)?.Slots.Where(s => blade != null && s.Item == blade.Id && !s.Ref.IsNone)
            .Select(s => Items.GearOf(s.Ref)?.Rarity).ToList() ?? [];
        c.Expect(Count() - start >= 10, "ten blades were crafted", $"{Count() - start} new");
        c.Log.Info($"client-pass: equip.c10 rarities: {string.Join(", ", made.GroupBy(r => r).Select(g => $"{g.Key} x {g.Count()}"))}");
        c.Expect(made.Distinct().Count() > 1, "the crafted blades are not all one rarity (50/30/15/4/1 weights)",
            string.Join(", ", made.GroupBy(r => r).Select(g => $"{g.Key} x {g.Count()}")));
        c.Instruct("Look at the crafted blades in your inventory. Pass only if both held: (1) their rarity colours and enhancement rows " +
                   "differ (mostly Common and Uncommon, some Rare); (2) the recipe's tooltip showed Common before each craft. Otherwise " +
                   "Fail with a note naming which part.");
        await c.Verdict();
    }

    [ClientCheck("equip.c11", "A rarity written by a mod, up and down, is seen at once by the holder", Area = EquipArea, Since = EquipSince,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7015, Confirm = EquipConfirm)]
    public async Task EquipSetRaritySeen(CheckContext c)
    {
        if (!await NeedEquip(c, "items.gear.write")) return;
        var chest = await GiveGear(c, FivePerkChest, ItemRarity.Rare);
        if (chest.IsNone) { await c.Verdict(); return; }
        var watch = WatchDetail(c, Items.Lookup(FivePerkChest)?.Id ?? default);
        c.Instruct("Open your inventory and look at the new Rare Warrior chest piece's tooltip (three enhancement rows) and keep it open. " +
                   Then(watch, "its tooltip has shown for five seconds", 45));
        await UntilViews(c, watch, 1, "the chest piece's tooltip shown", 40, linger: 5);
        var up = await Items.SetRarity(chest, ItemRarity.Legendary);
        c.Expect(up.Ok, "the chest piece was set to Legendary", up.ToString());
        c.Instruct("The chest piece is now Legendary. Without closing anything, remember whether within a second the tooltip shows the " +
                   "Legendary colour and five enhancement rows (move the pointer off the item and back if it does not redraw by itself). " +
                   "The check sets it to Common by itself in 20 seconds.");
        await c.Delay(TimeSpan.FromSeconds(20));
        var down = await Items.SetRarity(chest, ItemRarity.Common);
        c.Expect(down.Ok && down.After is { Enhancements: 1 }, "the chest piece was set to Common", down.ToString());
        c.Instruct("The chest piece is now Common. Equip it and take it off once. Pass only if all held: (1) the Legendary colour and five " +
                   "rows showed within a second; (2) it now shows the Common colour and one enhancement row and still sits in the same " +
                   "slot (the same item, not a new one); (3) the armour value in the character screen is what a Common piece gives. " +
                   "Otherwise Fail with a note naming which part.");
        await c.Verdict();
    }
}
