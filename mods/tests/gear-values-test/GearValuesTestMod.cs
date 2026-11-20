// SPDX-License-Identifier: MIT
// Lab test of per-instance gear values (the equipment wave, package P7), player-free, over two boots of one lab. Boot 1:
// an armour piece and a sword in a chest; a level outside the armour's range (refused without AnyLevel, written and kept
// with it), a max durability (the share kept), the sword's power and the armour's armour multiplied (the first scale of
// each kind installs kiln.items' hooks); then a save, and a marker in the mod's data folder. Boot 2 (the marker is there):
// the items are found again in the chest by its place, and every value must be back: the game set the level back into the
// range and computed max durability again at the load, kiln.items' scale table started empty, and Kiln.Blocks' keeper
// writes them again from the item data. Lines "PASS/FAIL ..." and "gear-values-test done: N PASS, M FAIL" in kiln.log.
using System.Globalization;
using Kiln;

namespace GearValuesTest;

public sealed class GearValuesTestMod : Mod
{
    private const string Armour = "Armor_T2_Warrior_Tank_Chest";  // Rare, five perk slots, one level (the dossier)
    private const string Sword = "Weapon_T1_1H_Sword_Craft";      // Common, levels 5..5, a damage setup

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
            Log.Warn("gear-values-test: needs kiln.items");
            return;
        }
        Context.Tick.Pre += (in TickInfo t) =>
        {
            if (_ran || t.Step % 25 != 0 || !Lifecycle.IsServerRunning || !Items.GearWriteAvailable) return;
            _ran = true;
            _ = Run();
        };
    }

    private async Task Run()
    {
        var marker = Path.Combine(Context.DataDirectory, "persist.txt");
        try
        {
            if (File.Exists(marker)) await AfterRestart(marker);
            else await FirstBoot(marker);
        }
        catch (Exception e)
        {
            Check(false, $"the test threw: {e}");
        }
        finally
        {
            Log.Info($"gear-values-test done: {_pass} PASS, {_fail} FAIL");
        }
    }

    private async Task<ItemInstanceRef> GiveOne(EntityId box, string name, Gear gear)
    {
        var r = await Items.Give(box, Items.Lookup(name)!.Id, 1, gear);
        return r.Changes.FirstOrDefault(c => !c.Ref.IsNone).Ref;
    }

    private async Task<EntityId> Chest()
    {
        foreach (var e in Items.FindInventories(InventoryKinds.Containers))
            if (Items.Read(e) is { Initialized: true } c && c.FreeSlots() >= 3 && World.Position(e) is not null) return e;
        var bases = World.Find("BaseIdComponent");
        var near = bases.Count > 0 && World.Position(bases[0]) is { } b ? b with { X = b.X + 6 } : new WorldPosition(3806, 834, 1434);
        var chest = await Spawn.Entity("LootChest_Random_Cairn_Sarcophagus_Wood", near);
        await Steps(30);
        Log.Info($"gear-values-test: spawned chest {chest} at {near}");
        return chest;
    }

    private async Task FirstBoot(string marker)
    {
        await Steps(25 * 3);
        var chest = await Chest();
        var armour = await GiveOne(chest, Armour, new Gear(Rarity: ItemRarity.Rare));
        var sword = await GiveOne(chest, Sword, new Gear());
        await Steps(3);  // the game computes durability in the items' first step
        var a0 = Items.GearOf(armour);
        var s0 = Items.GearOf(sword);
        Check(a0 is not null && s0 is not null, $"an armour piece and a sword in chest {chest}: {a0}; {s0}");
        if (a0 is null || s0 is null) return;
        Log.Info($"gear-values-test: armour levels {a0.TypeLevelMin}..{a0.TypeLevelMax}, sword levels {s0.TypeLevelMin}..{s0.TypeLevelMax}");

        var want = a0.TypeLevelMax + 30;
        var refused = await Items.SetLevel(armour, want);
        Check(refused.Status == GearStatus.OutOfRange, $"level {want} without AnyLevel is refused: {refused}");
        var lv = await Items.SetLevel(armour, want, new GearWriteOptions { AnyLevel = true });
        Check(lv.Ok && lv.Kept && lv.After?.Level == want, $"level {want} with AnyLevel is written and kept: {lv}");
        await Steps(2);
        Check(Items.GearOf(armour) is { } a1 && a1.Level == want && a1.ShownLevel == want, $"the game derives the shown level {want}: {Items.GearOf(armour)}");

        // armour pieces have no durability in this game: the sword's (the armour's write is a NotGear refusal)
        var none = await Items.SetDurabilityMax(armour, 900);
        Check(none.Status == GearStatus.NotGear, $"an armour piece has no durability: {none}");
        var dur = s0.Durability ?? 0;
        var dm = await Items.SetDurabilityMax(sword, 900);
        Check(dm.Ok && dm.Kept && dm.After?.DurabilityMax == 900 && dm.After.Durability == 900 * dur / Math.Max(1, s0.DurabilityMax ?? 1),
            $"the sword's max durability 900, the share kept: {dm}");
        await Steps(5);
        Check(Items.GearOf(sword)?.DurabilityMax == 900, $"the game keeps max durability 900 in the session (it computes it once): {Items.GearOf(sword)} (was {dur}/{s0.DurabilityMax})");

        var p = Items.SetPower(sword, 2.0);
        Check(p.Ok && p.Power == 2.0 && p.Kept, $"the sword's power x2 (the detour installs now): {p}");
        var ar = Items.SetArmor(armour, 1.5);
        Check(ar.Ok && ar.Armor == 1.5 && ar.Kept, $"the armour's armour x1.5 (the mid hooks install now): {ar}");
        Check(Items.ValuesOf(sword) is { Power: 2.0, Armor: 1.0, KeptDurabilityMax: 900 } && Items.ValuesOf(armour) is { Armor: 1.5, KeptBaseLevel: not null },
            $"read back: {Items.ValuesOf(sword)}; {Items.ValuesOf(armour)}");
        Log.Info($"gear-values-test: rows power {Capabilities.Is("items.gear.power")}, armor {Capabilities.Is("items.gear.armor")} ({Capabilities.Why("items.gear.armor")})");

        await Steps(30);  // a few armour passes and impact starts with the hooks in: the server keeps running
        Check(Lifecycle.IsServerRunning && Items.GearOf(armour) is not null, "the server runs on with the hooks installed");

        var saved = await Saves.Request();
        Log.Info($"gear-values-test: save {saved}");
        var pos = World.Position(chest) ?? default;
        Directory.CreateDirectory(Path.GetDirectoryName(marker)!);
        File.WriteAllText(marker, string.Join(' ', want, pos.X.ToString(CultureInfo.InvariantCulture), pos.Y.ToString(CultureInfo.InvariantCulture),
            pos.Z.ToString(CultureInfo.InvariantCulture)));
        Log.Info("gear-values-test: RESTART the lab now: the next start checks the values came back");
    }

    private async Task AfterRestart(string marker)
    {
        var parts = File.ReadAllText(marker).Split(' ');
        File.Delete(marker);
        var want = int.Parse(parts[0], CultureInfo.InvariantCulture);
        var at = parts.Skip(1).Select(x => double.Parse(x, CultureInfo.InvariantCulture)).ToArray();
        ItemInstanceRef armour = default, sword = default;
        GearState? first = null;
        for (var tries = 0; tries < 40 && (armour.IsNone || sword.IsNone); tries++)
        {
            await Steps(10);
            foreach (var e in Items.FindInventories(InventoryKinds.Containers))
            {
                if (World.Position(e) is not { } q || Math.Abs(q.X - at[0]) + Math.Abs(q.Y - at[1]) + Math.Abs(q.Z - at[2]) > 3) continue;
                foreach (var s in Items.Read(e)?.Slots ?? [])
                {
                    if (s.Ref.IsNone) continue;
                    var name = Items.Get(s.Item)?.Name;
                    if (name == Armour && armour.IsNone) armour = s.Ref;
                    if (name == Sword && sword.IsNone) sword = s.Ref;
                }
            }
            if (!armour.IsNone && first is null) first = Items.GearOf(armour);
        }
        Check(!armour.IsNone && !sword.IsNone, $"after the restart the armour and the sword are in the chest near ({at[0]}, {at[1]}, {at[2]})");
        if (armour.IsNone || sword.IsNone) return;
        Log.Info($"gear-values-test: the armour as first read after the load: {first}");
        Log.Info($"gear-values-test: the sword as first read after the load: {Items.GearOf(sword)}");
        GearState? g = null, sw = null;
        for (var i = 0; i < 12; i++)  // the keeper's pass runs about once a second
        {
            await Steps(25);
            g = Items.GearOf(armour);
            sw = Items.GearOf(sword);
            if (g?.Level == want && sw?.DurabilityMax == 900 && Items.ValuesOf(armour)?.Armor == 1.5 && Items.ValuesOf(sword)?.Power == 2.0) break;
        }
        Check(g?.Level == want, $"the level outside the range is back: {g}");
        Check(sw?.DurabilityMax == 900, $"the sword's max durability 900 is back: {sw}");
        Check(Items.ValuesOf(armour) is { Armor: 1.5, Kept: true }, $"the armour multiplier is back: {Items.ValuesOf(armour)}");
        Check(Items.ValuesOf(sword) is { Power: 2.0, Kept: true }, $"the power multiplier is back: {Items.ValuesOf(sword)}");
    }
}
