// SPDX-License-Identifier: MIT
// Lab test of upgrade stations (InteractBlock.Upgrade, UpgradeStations), player-free: the "player" is a chest (an upgrade needs
// someone to run for, and its payer is a container), and its hand is the test's. Lines "PASS/FAIL ..." and
// "upgrade-station-test done: N PASS, M FAIL" in kiln.log.
using Kiln;

namespace UpgradeStationTest;

public sealed class UpgradeStationTestMod : Mod
{
    private const string Me = "upgrade-station-test";
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
        if (!Items.Available || !Content.Available || !World.Available)
        {
            Log.Warn("upgrade-station-test: needs kiln.items, kiln.content, kiln.world");
            return;
        }
        Blocks.Define("lab-upgrades", new RarityUpgradeBlock
        {
            Items = [Armour],
            Steps =
            {
                [Rarity.Epic] = new UpgradeStep { Cost = { [Twigs] = 10 } },
                [Rarity.Legendary] = new UpgradeStep { Cost = { [Twigs] = 5 }, Chance = 0 },
            },
        });
        Blocks.Define("lab-anvil", new PropBlock
        {
            Name = "Lab Anvil", Visual = new VisualBlock { Like = "Prop_Decoration_Comfort1_T0_Table_Medium" },
            OnInteract = new InteractBlock { Prompt = "Upgrade held item", Upgrade = new UpgradeAction { Rule = "lab-upgrades" } },
        });
        Blocks.Define("lab-box", new ContainerBlock { Name = "Lab Box", Slots = 16 });
        Blocks.Define("lab-forge", new PropBlock
        {
            Name = "Lab Forge", Visual = new VisualBlock { Like = "Prop_Decoration_Comfort1_T0_Table_Medium" },
            OnInteract = new InteractBlock { Prompt = "Upgrade gear", Upgrade = new UpgradeAction { Rule = "lab-upgrades", Target = UpgradeTarget.Choose, Range = 2 } },
        });
        Context.Tick.Pre += (in TickInfo t) =>
        {
            if (_ran || t.Step % 25 != 0 || !Lifecycle.IsServerRunning || !Items.UpgradeAvailable) return;
            _ran = true;
            _ = Run();
        };
    }

    private static readonly BuildOptions InBases = new() { Flags = BuildFlags.InBases };
    private readonly List<EntityId> _placed = [];

    // the ground's top at a spot open in a fresh lab world (furniture-test's)
    private static async Task<WorldPosition> OpenGround(double x = 3024, double z = 1550)
    {
        var box = Build.BoxBetween(new WorldPosition(x, 700, z), new WorldPosition(x, 1100, z));
        var cells = await Build.ReadAsync(box);
        for (var y = box.SizeY - 1; y >= 0; y--)
        {
            var c = cells[box.Index(0, y, 0)];
            if (c.Fill > 0 && c.Material is > 0 and < 128) return new WorldPosition(x, Build.CornerOf(new CellPos(box.X, box.Y + y, box.Z)).Y + c.Fill / 255.0 * 0.5 + 0.05, z);
        }
        throw new InvalidOperationException("no ground at the open spot");
    }

    private static int Count(EntityId holder, ItemId item) => Items.Read(holder)?.Slots.Where(s => s.Item == item).Sum(s => s.Count) ?? -1;

    private static UpgradeStations.Station? StationOf(string block)
    {
        var t = Content.Templates.Get($"{Me}.{block}.prop");
        return t == null ? null : InteractRuntime.EntryOf(t.Guid)?.Upgrade;
    }

    private async Task Run()
    {
        var told = new List<(EntityId Who, UpgradeNotice Notice)>();
        var menus = new List<UpgradeMenu>();
        ItemInstanceRef held = default;
        try
        {
            await Steps(25 * 3);
            // 1: what the blocks became
            var anvilT = Content.Templates.Get($"{Me}.lab-anvil.prop");
            Check(anvilT != null && (uint)anvilT.Get("InteractionOfferRuntime.defaultSequence").Integer == InteractPlay.Toggle.Id,
                $"the anvil's template plays the hand on it: {anvilT?.Get("InteractionOfferRuntime.defaultSequence")}");
            var anvil = StationOf("lab-anvil");
            var forge = StationOf("lab-forge");
            Check(anvil is { Rule: $"{Me}:lab-upgrades", Target: UpgradeTarget.Held }, $"the anvil's interaction runs the rule on the held item: {anvil}");
            Check(forge is { Target: UpgradeTarget.Choose, Range: 2 }, $"the forge's interaction opens the window within 2 m: {forge}");
            Check(UpgradeStation.Available, $"blocks.upgrade_station is on: {Capabilities.Why(UpgradeStation.Capability)}");
            if (anvil == null || forge == null) return;

            // two boxes of this mod's own, 10 m apart on open ground (a fresh lab world has no chest)
            var ground = await OpenGround();
            var a = Build.PlaceProp($"{Me}.lab-box.prop", ground, null, 0, InBases);
            var b = Build.PlaceProp($"{Me}.lab-box.prop", new WorldPosition(ground.X + 10, ground.Y, ground.Z), null, 0, InBases);
            _placed.AddRange([a, b]);
            await Steps(20);
            var boxesOk = Items.Read(a) is { Initialized: true } && Items.Read(b) is { Initialized: true };
            Check(boxesOk, $"two boxes placed with their inventories: {a}, {b}");
            if (!boxesOk) return;
            var twigs = Items.Lookup(Twigs)!.Id;
            UpgradeStations.HeldOf = _ => held;
            UpgradeStations.SendNotice = (who, n) => { told.Add((who, n)); Log.Info($"upgrade-station-test: told {who}: {n.Text}"); };
            UpgradeStations.SendMenu = (_, m) => menus.Add(m);
            UpgradeStations.CanChoose = _ => false;

            // 2: the held item: upgraded in place and paid by the chest standing in for the player, in one step
            var give = await Items.Give(a, Items.Lookup(Armour)!.Id, 1, new Gear(Rarity: ItemRarity.Rare));
            held = give.Changes.FirstOrDefault(c => !c.Ref.IsNone).Ref;
            await Items.Give(a, twigs, 10);
            var before = Count(a, twigs);
            var r1 = await UpgradeStations.Use(a, a, anvil);
            await Steps(2);
            Check(r1 is { Status: UpgradeStatus.Done } && Items.GearOf(held)?.Rarity == ItemRarity.Epic && Count(a, twigs) == before - 10,
                $"the anvil upgraded the held armour Rare -> Epic, paid 10 twigs: {r1}; now {Items.GearOf(held)}, twigs {Count(a, twigs)} (was {before})");
            Check(told.Count == 1 && told[0].Notice.Ok && told[0].Notice.Text.Contains("is now Epic"), $"the player is told: {(told.Count > 0 ? told[^1].Notice.Text : "nothing")}");

            // 3: an empty hand: nothing happens, the player is told
            held = default;
            var r2 = await UpgradeStations.Use(a, a, anvil);
            Check(r2 == null && told[^1].Notice.Text.Contains("Hold the item"), $"an empty hand: {told[^1].Notice.Text}");

            // 4: the window: priced by a dry run, nothing paid; the pick upgrades (chance 0: fails, the cost spent)
            UpgradeStations.CanChoose = _ => true;
            await Items.Give(a, twigs, 5);
            var before4 = Count(a, twigs);
            var r3 = await UpgradeStations.Use(a, a, forge);
            await Steps(2);
            var menu = menus.LastOrDefault();
            var row = menu?.Rows.FirstOrDefault(x => x.Item == Items.Lookup(Armour)!.Id.Value);
            Check(r3 == null && row is { Can: true, To: (byte)ItemRarity.Legendary } && Count(a, twigs) == before4,
                $"the window lists the armour Epic -> Legendary for {row?.Cost}, nothing paid: {menu?.Rows.Length} row(s)");
            if (menu != null && row != null)
            {
                var pick = new UpgradePick(menu.Token, row.Entity, row.Serial, row.Item);
                var r4 = await UpgradeStations.Pick(a, pick);
                Check(r4 is { Status: UpgradeStatus.Failed } && Count(a, twigs) == before4 - 5 && told[^1].Notice.Text.Contains("failed"),
                    $"the pick ran the upgrade (chance 0: failed, cost spent): {r4}; twigs {Count(a, twigs)}");
                Check(menus.Count >= 2 && menus[^1].Token != menu.Token, "the window was sent again with a new token");

                // 5: a pick from the old window, and one from too far away, are refused
                var r5 = await UpgradeStations.Pick(a, pick);
                Check(r5 == null && told[^1].Notice.Text.Contains("closed"), $"the old window's pick is refused: {told[^1].Notice.Text}");
                var far = World.Position(a) is { } pa && World.Position(b) is { } pb
                    ? Math.Sqrt((pa.X - pb.X) * (pa.X - pb.X) + (pa.Y - pb.Y) * (pa.Y - pb.Y) + (pa.Z - pb.Z) * (pa.Z - pb.Z)) : 0;
                if (far > forge.Range)
                {
                    await Items.Give(a, twigs, 5);           // the price again, so the window offers the armour
                    await UpgradeStations.Use(a, b, forge);  // the window of a station that stands at the other box
                    var m2 = menus[^1];
                    var row2 = m2.Rows.First(x => x.Item == row.Item);
                    var r6 = await UpgradeStations.Pick(a, new UpgradePick(m2.Token, row2.Entity, row2.Serial, row2.Item));
                    Check(r6 == null && told[^1].Notice.Text.Contains("too far"), $"a pick {far:F1} m from the station is refused: {told[^1].Notice.Text}");
                }
                else Log.Info($"upgrade-station-test: the two chests are {far:F1} m apart, within the forge's range: the range check is the offline test's");
            }
        }
        catch (Exception e)
        {
            Check(false, $"the test threw: {e}");
        }
        finally
        {
            UpgradeStations.HeldOf = static p => Items.Held(p);
            UpgradeStations.SendNotice = static (p, n) => UpgradeNet.Notice(p, n);
            UpgradeStations.SendMenu = static (p, m) => UpgradeNet.Menu(p, m);
            UpgradeStations.CanChoose = static p => UpgradeNet.CanChoose(p);
            foreach (var e in _placed)
                if (World.Exists(e)) Build.RemoveProp(e, InBases);
            Log.Info($"upgrade-station-test done: {_pass} PASS, {_fail} FAIL");
        }
    }
}
