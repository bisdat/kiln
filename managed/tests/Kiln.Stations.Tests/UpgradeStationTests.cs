// SPDX-License-Identifier: MIT
// The upgrade station's use (package P4 of the equipment wave) on the test kit's kiln.items: a held item upgraded and paid in
// one step and the player told; an empty hand; the rule's refusals told; the window for a Kiln player (priced by dry runs,
// nothing paid), its pick taken only from the list it sent, from a player still near the station, for an item still
// carried, then sent again with the new prices; a player whose game cannot show the window gets the held item; the row.
// What the game shows is the client checks equip.c1 to equip.c4 (mods/tests/client-pass/EquipStation.cs).
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.StationsTests;

public sealed class UpgradeStationTests : IDisposable
{
    private const ulong AdaSteam = 76561198000000931;
    private const string Mod = "anvil-test";

    private readonly KilnHarness _kiln = new();
    private readonly FakePlayer _ada;
    private readonly ItemId _sword, _scraps;
    private readonly EntityId _anvil = new(0x7001);
    private readonly List<UpgradeNotice> _told = [];
    private int _offTick;  // notices sent off the tick thread (Chat.Tell would throw there)
    private readonly int _tickThread;
    private readonly List<UpgradeMenu> _menus = [];
    private ItemInstanceRef _held;
    private bool _kilnClient;
    private WorldPosition _player = new(1, 0, 0);

    public UpgradeStationTests()
    {
        _sword = _kiln.Items.AddItem("Weapon_T1_1H_Sword_Anvil", 1, ItemCategory.Weapons, gear: true, rarity: ItemRarity.Common, levelMin: 5, levelMax: 5);
        _scraps = _kiln.Items.AddItem("Material_Scraps_Anvil", 100);
        _ada = _kiln.Players.Join("Ada", AdaSteam, position: new WorldPosition(0, 0, 0));
        _kiln.Step(2);
        _tickThread = _kiln.Run(() => Environment.CurrentManagedThreadId);
        var block = new RarityUpgradeBlock
        {
            Items = ["Weapon_*"],
            Max = Rarity.Epic,
            Steps =
            {
                [Rarity.Uncommon] = new UpgradeStep { Cost = { ["Material_Scraps_Anvil"] = 10 } },
                [Rarity.Rare] = new UpgradeStep { Cost = { ["Material_Scraps_Anvil"] = 10 } },
                [Rarity.Epic] = new UpgradeStep { Cost = { ["Material_Scraps_Anvil"] = 10 }, Chance = 0.5 },
            },
        };
        ItemUpgrades.Register($"{Mod}:upgrades", RarityUpgradeRuntime.RuleOf(Mod, "upgrades", block));
        UpgradeStations.Reset();
        UpgradeStations.HeldOf = _ => _held;
        UpgradeStations.SendNotice = (_, n) =>
        {
            if (Environment.CurrentManagedThreadId != _tickThread) _offTick++;
            _told.Add(n);
        };
        UpgradeStations.SendMenu = (_, m) => _menus.Add(m);
        UpgradeStations.CanChoose = _ => _kilnClient;
        UpgradeStations.PositionOf = e => e == _anvil ? new WorldPosition(0, 0, 0) : e == _ada.Entity ? _player : null;
    }

    public void Dispose()
    {
        UpgradeStations.HeldOf = static p => Items.Held(p);
        UpgradeStations.SlotsOf = static p => Items.Read(p)?.Slots ?? [];
        UpgradeStations.SendNotice = static (p, n) => UpgradeNet.Notice(p, n);
        UpgradeStations.SendMenu = static (p, m) => UpgradeNet.Menu(p, m);
        UpgradeStations.CanChoose = static p => UpgradeNet.CanChoose(p);
        UpgradeStations.PositionOf = static e => World.Exists(e) ? World.Position(e) : null;
        UpgradeStations.Reset();
        ItemUpgrades.Forget(Mod);
        ItemUpgrades.Roller = null;
        _kiln.Dispose();
    }

    private static UpgradeStations.Station Station(UpgradeTarget target = UpgradeTarget.Held) =>
        new(Mod, "anvil", $"{Mod}:upgrades", target, UpgradeAction.DefaultRange, "Rarity Anvil");

    private ItemInstanceRef GiveSword(ItemRarity rarity)
    {
        var give = _kiln.Run(() => Items.Give(_ada.Entity, _sword, 1, new Gear(5, rarity)));
        _kiln.Step();
        Assert.True(give.Result.Succeeded, give.Result.ToString());
        return Assert.Single(give.Result.Changes).Ref;
    }

    private void GiveScraps(int n)
    {
        var give = _kiln.Run(() => Items.Give(_ada.Entity, _scraps, n));
        _kiln.Step();
        Assert.True(give.Result.Succeeded, give.Result.ToString());
    }

    private int Scraps => _kiln.Items.Of(_ada.Entity)!.Count(_scraps);

    private ItemRarity RarityOf(ItemInstanceRef item) => _kiln.Run(() => Items.GearOf(item))!.Rarity;

    private T Wait<T>(Func<Task<T>> call)
    {
        var t = _kiln.Run(call);
        for (var i = 0; i < 6 && !t.IsCompleted; i++) _kiln.Step();
        Assert.True(t.IsCompletedSuccessfully, t.Exception?.ToString() ?? "still running");
        return t.Result;
    }

    [Fact]
    public void The_row_is_on_where_upgrades_are()
    {
        Assert.True(_kiln.Run(() => UpgradeStation.Available), _kiln.Run(() => Capabilities.Why(UpgradeStation.Capability)));
    }

    [Fact]
    public void The_held_item_is_upgraded_and_paid_in_one_step_and_the_player_is_told()
    {
        _held = GiveSword(ItemRarity.Common);
        GiveScraps(15);
        var r = Wait(() => UpgradeStations.Use(_ada.Entity, _anvil, Station()));
        Assert.NotNull(r);
        Assert.Equal(UpgradeStatus.Done, r!.Status);
        Assert.Equal(ItemRarity.Uncommon, RarityOf(_held));
        Assert.Equal(5, Scraps);
        var told = Assert.Single(_told);
        Assert.True(told.Ok);
        Assert.StartsWith("Weapon_T1_1H_Sword_Anvil is now Uncommon: unlock its new enhancement at a workbench.", told.Text);
        Assert.Equal(0, _offTick);  // told after the upgrade's await, still on the tick thread
    }

    [Fact]
    public void An_empty_hand_upgrades_nothing_and_says_what_to_do()
    {
        _held = ItemInstanceRef.None;
        GiveScraps(15);
        var r = Wait(() => UpgradeStations.Use(_ada.Entity, _anvil, Station()));
        Assert.Null(r);
        Assert.Equal(15, Scraps);
        Assert.Contains("Hold the item", Assert.Single(_told).Text);
        Assert.False(_told[0].Ok);
    }

    [Fact]
    public void Too_little_to_pay_and_the_rules_top_are_told_and_nothing_is_paid()
    {
        _held = GiveSword(ItemRarity.Common);
        GiveScraps(4);
        var poor = Wait(() => UpgradeStations.Use(_ada.Entity, _anvil, Station()));
        Assert.Equal(UpgradeStatus.CantPay, poor!.Status);
        Assert.Equal(4, Scraps);
        Assert.StartsWith("Not enough to upgrade", _told[^1].Text);

        _held = GiveSword(ItemRarity.Epic);
        var top = Wait(() => UpgradeStations.Use(_ada.Entity, _anvil, Station()));
        Assert.Equal(UpgradeStatus.AtMax, top!.Status);
        Assert.Equal("Weapon_T1_1H_Sword_Anvil is Epic: this station upgrades nothing past that.", _told[^1].Text);
    }

    [Fact]
    public void A_failed_roll_is_told_with_its_chance()
    {
        _held = GiveSword(ItemRarity.Rare);
        GiveScraps(10);
        _kiln.Items.SetUpgradeRolls(0.9);
        var r = Wait(() => UpgradeStations.Use(_ada.Entity, _anvil, Station()));
        Assert.Equal(UpgradeStatus.Failed, r!.Status);
        Assert.Equal(ItemRarity.Rare, RarityOf(_held));
        Assert.Equal(0, Scraps);
        Assert.Equal("The upgrade of Weapon_T1_1H_Sword_Anvil to Epic failed (50% chance): it stays Rare, and 10 x Material_Scraps_Anvil are spent.", _told[^1].Text);
    }

    [Fact]
    public void A_player_whose_game_has_no_window_gets_the_held_item()
    {
        _held = GiveSword(ItemRarity.Common);
        GiveScraps(10);
        _kilnClient = false;
        var r = Wait(() => UpgradeStations.Use(_ada.Entity, _anvil, Station(UpgradeTarget.Choose)));
        Assert.Equal(UpgradeStatus.Done, r!.Status);
        Assert.Empty(_menus);
    }

    [Fact]
    public void The_window_lists_the_gear_priced_by_dry_runs_and_pays_nothing()
    {
        var common = GiveSword(ItemRarity.Common);
        var epic = GiveSword(ItemRarity.Epic);
        GiveScraps(12);
        _kilnClient = true;
        var r = Wait(() => UpgradeStations.Use(_ada.Entity, _anvil, Station(UpgradeTarget.Choose)));
        Assert.Null(r);
        Assert.Equal(12, Scraps);
        Assert.Empty(_told);
        var menu = Assert.Single(_menus);
        Assert.Equal("Rarity Anvil", menu.Title);
        Assert.NotEqual(0u, menu.Token);
        var can = Assert.Single(menu.Rows, x => x.Entity == common.Entity.Id.Value);
        Assert.True(can.Can);
        Assert.Equal(((byte)ItemRarity.Common, (byte)ItemRarity.Uncommon), (can.From, can.To));
        Assert.Equal("10 x Material_Scraps_Anvil", can.Cost);
        var cannot = Assert.Single(menu.Rows, x => x.Entity == epic.Entity.Id.Value);
        Assert.False(cannot.Can);
        Assert.Contains("nothing past that", cannot.Note);
    }

    private UpgradeMenu OpenWindow()
    {
        _kilnClient = true;
        Wait(() => UpgradeStations.Use(_ada.Entity, _anvil, Station(UpgradeTarget.Choose)));
        return _menus[^1];
    }

    private static UpgradePick PickOf(UpgradeMenu m, ItemInstanceRef item) => new(m.Token, item.Entity.Id.Value, item.Entity.Serial, item.Item.Value);

    [Fact]
    public void A_pick_from_the_window_upgrades_that_item_and_the_window_comes_again()
    {
        var sword = GiveSword(ItemRarity.Common);
        GiveScraps(25);
        var menu = OpenWindow();
        var r = Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, sword)));
        Assert.Equal(UpgradeStatus.Done, r!.Status);
        Assert.Equal(ItemRarity.Uncommon, RarityOf(sword));
        Assert.Equal(15, Scraps);
        Assert.True(_told[^1].Ok);
        Assert.Equal(2, _menus.Count);
        var again = _menus[^1];
        Assert.NotEqual(menu.Token, again.Token);
        Assert.Equal((byte)ItemRarity.Rare, Assert.Single(again.Rows).To);
        // the old window's token is no longer taken
        var stale = Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, sword)));
        Assert.Null(stale);
        Assert.Equal(ItemRarity.Uncommon, RarityOf(sword));
    }

    [Fact]
    public void A_pick_the_window_did_not_offer_is_ignored()
    {
        var sword = GiveSword(ItemRarity.Common);
        var top = GiveSword(ItemRarity.Epic);
        GiveScraps(25);
        var menu = OpenWindow();
        Assert.Null(Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, top))));
        Assert.Null(Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, sword) with { Entity = 0xdead })));
        Assert.Null(Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, sword) with { Token = menu.Token + 1 })));
        Assert.Equal(25, Scraps);
        Assert.Equal(ItemRarity.Common, RarityOf(sword));
    }

    [Fact]
    public void A_pick_from_too_far_away_is_refused_and_ends_the_window()
    {
        var sword = GiveSword(ItemRarity.Common);
        GiveScraps(25);
        var menu = OpenWindow();
        _player = new WorldPosition(20, 0, 0);
        Assert.Null(Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, sword))));
        Assert.Contains("too far", _told[^1].Text);
        _player = new WorldPosition(1, 0, 0);
        Assert.Null(Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, sword))));  // the window ended with it
        Assert.Contains("closed", _told[^1].Text);
        Assert.Equal(25, Scraps);
    }

    [Fact]
    public void A_pick_of_an_item_no_longer_carried_is_refused()
    {
        var sword = GiveSword(ItemRarity.Common);
        GiveScraps(25);
        var menu = OpenWindow();
        UpgradeStations.SlotsOf = _ => [];
        Assert.Null(Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, sword))));
        Assert.Contains("no longer in your inventory", _told[^1].Text);
        Assert.Equal(ItemRarity.Common, RarityOf(sword));
    }

    [Fact]
    public void A_window_closes_after_its_time()
    {
        long now = 1000;
        UpgradeStations.Clock = () => now;
        try
        {
            var sword = GiveSword(ItemRarity.Common);
            GiveScraps(25);
            var menu = OpenWindow();
            now += (long)UpgradeStations.MenuLife.TotalMilliseconds + 1;
            Assert.Null(Wait(() => UpgradeStations.Pick(_ada.Entity, PickOf(menu, sword))));
            Assert.Contains("closed", _told[^1].Text);
        }
        finally { UpgradeStations.Clock = static () => Environment.TickCount64; }
    }

    [Fact]
    public void The_code_path_checks_its_action()
    {
        _kiln.Run(() =>
        {
            Assert.Throws<ArgumentException>(() => { _ = UpgradeStation.Use(EntityId.None, _anvil, new UpgradeAction { Rule = "upgrades" }); });
            Assert.Throws<ArgumentException>(() => { _ = UpgradeStation.Use(_ada.Entity, _anvil, new UpgradeAction { Rule = "upgrades", Range = 0 }); });
        });
        Assert.Throws<InvalidOperationException>(() => { _ = UpgradeStation.Use(_ada.Entity, _anvil, new UpgradeAction { Rule = "upgrades" }); });
    }
}
