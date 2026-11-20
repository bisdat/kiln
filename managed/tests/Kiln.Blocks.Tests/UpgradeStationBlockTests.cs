// SPDX-License-Identifier: MIT
// The upgrade station (package P4 of the equipment wave) with no game: InteractBlock.Upgrade lowered to the template's fields and to
// what the block became, the mistakes refused by field, the JSON form, the rule's id, the window's messages on the wire, the
// distance check, the capability row and what a player is told for each outcome. The whole use on the test kit's kiln.items:
// managed/tests/Kiln.Stations.Tests/UpgradeStationTests.cs; in a game: mods/tests/upgrade-station-test and the client checks
// equip.c1 to equip.c4.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class UpgradeStationBlockTests : IDisposable
{
    private static ModBlocks Mod(string id) => new(id, 1) { ManifestClients = "kiln" };

    private static JsonObject Lower(PropBlock b) => Compiler.Lower(Mod("anvil-mod"), "anvil", b).Low.Json;

    private static PropBlock Anvil(InteractBlock i) => new()
    {
        Name = "Rarity Anvil",
        Visual = new VisualBlock { Like = "Prop_Decoration_Comfort1_T0_Table_Medium" },
        OnInteract = i,
    };

    private static string Field(InteractBlock i) => Assert.Throws<BlockError>(() => Lower(Anvil(i))).Field ?? "";

    public void Dispose()
    {
        UpgradeStations.PositionOf = static e => World.Exists(e) ? World.Position(e) : null;
        UpgradeStations.Reset();
    }

    [Fact]
    public void An_upgrade_lowers_to_the_hand_on_it_and_names_its_rule_in_the_mod()
    {
        var j = Lower(Anvil(new InteractBlock { Prompt = "Upgrade", Upgrade = new UpgradeAction { Rule = "standard-upgrades", Target = UpgradeTarget.Choose, Range = 4 } }));
        var up = j["interact"]!["upgrade"]!;
        Assert.Equal("anvil-mod:standard-upgrades", (string?)up["rule"]);
        Assert.Equal("choose", (string?)up["target"]);
        Assert.Equal(4.0, (double)up["range"]!);
        var t = Assert.Single(j["templates"]!.AsArray())!.AsObject();
        Assert.Equal(InteractPlay.Toggle.Id.ToString(), t["fields"]!["InteractionOfferRuntime.defaultSequence"]!.ToString());
        Assert.Contains("InteractionOfferRuntime", t["addComponents"]!.AsArray().Select(n => (string?)n));
    }

    [Fact]
    public void Another_mods_rule_keeps_its_name_and_plays_wins_over_the_hand()
    {
        var j = Lower(Anvil(new InteractBlock { Plays = InteractPlay.Read, Upgrade = new UpgradeAction { Rule = "other-mod:upgrades" } }));
        Assert.Equal("other-mod:upgrades", (string?)j["interact"]!["upgrade"]!["rule"]);
        Assert.Equal("held", (string?)j["interact"]!["upgrade"]!["target"]);
        var t = Assert.Single(j["templates"]!.AsArray())!.AsObject();
        Assert.Equal(InteractPlay.Read.Id.ToString(), t["fields"]!["InteractionOfferRuntime.defaultSequence"]!.ToString());
    }

    [Fact]
    public void A_wrong_upgrade_is_refused_by_field()
    {
        Assert.Equal("onInteract.upgrade.rule", Field(new InteractBlock { Upgrade = new UpgradeAction { Rule = " " } }));
        Assert.Equal("onInteract.upgrade.range", Field(new InteractBlock { Upgrade = new UpgradeAction { Rule = "r", Range = 0 } }));
        Assert.Equal("onInteract.upgrade.range", Field(new InteractBlock { Upgrade = new UpgradeAction { Rule = "r", Range = double.NaN } }));
        Assert.Equal("onInteract.upgrade.target", Field(new InteractBlock { Upgrade = new UpgradeAction { Rule = "r", Target = (UpgradeTarget)7 } }));
        Assert.Equal("onInteract.upgrade", Field(new InteractBlock { Toggle = new ToggleBlock(), Upgrade = new UpgradeAction { Rule = "r" } }));
    }

    [Fact]
    public void The_json_form_is_the_record()
    {
        var i = BlockJson.Parse<InteractBlock>("""{ "prompt": "Upgrade", "upgrade": { "rule": "standard-upgrades", "target": "choose", "range": 8 } }""");
        Assert.Equal(("standard-upgrades", UpgradeTarget.Choose, 8.0), (i.Upgrade!.Rule, i.Upgrade.Target, i.Upgrade.Range));
        var held = BlockJson.Parse<InteractBlock>("""{ "upgrade": { "rule": "r" } }""");
        Assert.Equal((UpgradeTarget.Held, UpgradeAction.DefaultRange), (held.Upgrade!.Target, held.Upgrade.Range));
        Assert.ThrowsAny<System.Text.Json.JsonException>(() => BlockJson.Parse<InteractBlock>("""{ "upgrade": { "target": "held" } }"""));
        Assert.ThrowsAny<System.Text.Json.JsonException>(() => BlockJson.Parse<InteractBlock>("""{ "upgrade": { "rule": "r", "cost": 3 } }"""));
    }

    [Fact]
    public void The_rule_id_and_the_actions_checks()
    {
        Assert.Equal("m:r", UpgradeStations.RuleId("r", "m"));
        Assert.Equal("o:r", UpgradeStations.RuleId(" o:r ", "m"));
        Assert.Equal("r", UpgradeStations.RuleId("r", null));
        UpgradeStations.Check(new UpgradeAction { Rule = "r" }, "a");
        Assert.Contains("Range", Assert.Throws<ArgumentException>(() => UpgradeStations.Check(new UpgradeAction { Rule = "r", Range = 65 }, "a")).Message);
        Assert.Contains("Rule", Assert.Throws<ArgumentException>(() => UpgradeStations.Check(new UpgradeAction { Rule = "" }, "a")).Message);
        Assert.Contains("Target", Assert.Throws<ArgumentException>(() => UpgradeStations.Check(new UpgradeAction { Rule = "r", Target = (UpgradeTarget)3 }, "a")).Message);
    }

    [Fact]
    public void The_window_and_its_pick_cross_the_wire_unchanged()
    {
        var menu = new UpgradeMenu(77, "Rarity Anvil",
        [
            new UpgradeMenuRow(12, 0x1_0000_0005, 0xabc, "Sword", 2, 3, true, 0.75f, "10 x Scraps", ""),
            new UpgradeMenuRow(13, 9, 0xabd, "Ring", 4, 4, false, 1f, "", "Ring is Legendary: this station upgrades nothing past that."),
        ]);
        var w = new NetWriter();
        UpgradeMenu.Encode(menu, w);
        var r = new NetReader(w.ToArray());
        var back = UpgradeMenu.Decode(ref r);
        Assert.Equal((menu.Token, menu.Title), (back.Token, back.Title));
        Assert.Equal(menu.Rows, back.Rows);
        Assert.True(r.End);

        var pick = new UpgradePick(77, 12, 0x1_0000_0005, 0xabc);
        var pw = new NetWriter();
        UpgradePick.Encode(pick, pw);
        var pr = new NetReader(pw.ToArray());
        Assert.Equal(pick, UpgradePick.Decode(ref pr));

        var notice = new UpgradeNotice("Sword is now Epic.", true);
        var nw = new NetWriter();
        UpgradeNotice.Encode(notice, nw);
        var nr = new NetReader(nw.ToArray());
        Assert.Equal(notice, UpgradeNotice.Decode(ref nr));
    }

    [Fact]
    public void A_window_lists_at_most_its_rows()
    {
        var rows = Enumerable.Range(0, UpgradeStations.MaxRows + 5).Select(i => new UpgradeMenuRow((uint)i + 1, 1, 1, "x", 0, 1, true, 1, "", "")).ToArray();
        var w = new NetWriter();
        UpgradeMenu.Encode(new UpgradeMenu(1, "t", rows), w);
        var r = new NetReader(w.ToArray());
        Assert.Equal(UpgradeStations.MaxRows, UpgradeMenu.Decode(ref r).Rows.Length);
    }

    [Fact]
    public void Near_measures_from_the_station_and_a_missing_one_is_never_near()
    {
        var at = new Dictionary<uint, WorldPosition> { [1] = new(0, 0, 0), [2] = new(3, 4, 0), [3] = new(10, 0, 0) };
        UpgradeStations.PositionOf = e => at.TryGetValue(e.Value, out var p) ? p : null;
        Assert.True(UpgradeStations.Near(new EntityId(2), new EntityId(1), 5));
        Assert.False(UpgradeStations.Near(new EntityId(2), new EntityId(1), 4.9));
        Assert.False(UpgradeStations.Near(new EntityId(3), new EntityId(1), 6));
        Assert.False(UpgradeStations.Near(new EntityId(2), new EntityId(99), 64));
    }

    [Fact]
    public void The_row_follows_the_upgrade_row()
    {
        Assert.Equal(CapabilityState.Available, UpgradeStations.RowOf(true, null).State);
        var off = UpgradeStations.RowOf(false, "kiln.items here has no upgrades");
        Assert.Equal(CapabilityState.Unavailable, off.State);
        Assert.Contains("items.gear.upgrade", off.Text);
        Assert.Contains("no upgrades", off.Text);
    }

    // ---- what a player is told ----

    private static readonly ItemId Sword = new(0x51), Scraps = new(0x52);

    private static string Name(ItemId id) => id == Sword ? "Iron Sword" : id == Scraps ? "Metal Scraps" : id.ToString();

    private static GearState Gear(ItemRarity r, int unlocked = 0) =>
        new(ItemInstanceRef.None, Sword, r, ItemRarity.Common, ItemRarity.Legendary, 5 + unlocked, 5, (int)r + 1, unlocked, 5 + (int)r + 1, false, null, null, []);

    private static UpgradeResult Result(UpgradeStatus s, ItemRarity from, ItemRarity to, GearState? before, GearState? after, int paid = 0, double chance = 1, string message = "") =>
        new(s, new EntityId(1), new ItemInstanceRef(new EntityRef(new EntityId(9), 1), Sword), from, to, before, after,
            paid > 0 ? new Dictionary<ItemId, int> { [Scraps] = paid } : new Dictionary<ItemId, int>(), chance, null, message);

    [Fact]
    public void A_success_says_the_new_rarity_and_where_to_unlock_it()
    {
        var t = UpgradeStations.Describe(Result(UpgradeStatus.Done, ItemRarity.Rare, ItemRarity.Epic, Gear(ItemRarity.Rare), Gear(ItemRarity.Epic), 10), Name);
        Assert.Equal("Iron Sword is now Epic: unlock its new enhancement at a workbench. Paid 10 x Metal Scraps.", t);
        var unlocked = UpgradeStations.Describe(Result(UpgradeStatus.Done, ItemRarity.Rare, ItemRarity.Epic, Gear(ItemRarity.Rare), Gear(ItemRarity.Epic, 1)), Name);
        Assert.Equal("Iron Sword is now Epic: its new enhancement is unlocked.", unlocked);
    }

    [Fact]
    public void A_failed_roll_says_what_it_cost_and_where_the_item_stands()
    {
        Assert.Equal("The upgrade of Iron Sword to Epic failed (50% chance): it stays Rare, and 10 x Metal Scraps are spent.",
            UpgradeStations.Describe(Result(UpgradeStatus.Failed, ItemRarity.Rare, ItemRarity.Epic, Gear(ItemRarity.Rare), Gear(ItemRarity.Rare), 10, 0.5), Name));
        Assert.Equal("The upgrade of Iron Sword to Epic failed (50% chance): it dropped to Uncommon, and 10 x Metal Scraps are spent.",
            UpgradeStations.Describe(Result(UpgradeStatus.Failed, ItemRarity.Rare, ItemRarity.Epic, Gear(ItemRarity.Rare), Gear(ItemRarity.Uncommon), 10, 0.5), Name));
        Assert.Equal("The upgrade of Iron Sword to Epic failed (50% chance): it stays Rare, and nothing was paid.",
            UpgradeStations.Describe(Result(UpgradeStatus.Failed, ItemRarity.Rare, ItemRarity.Epic, Gear(ItemRarity.Rare), Gear(ItemRarity.Rare), 0, 0.5), Name));
    }

    [Theory]
    [InlineData(UpgradeStatus.AtMax, "Iron Sword is Legendary: this station upgrades nothing past that.")]
    [InlineData(UpgradeStatus.NoSlot, "Iron Sword has no enhancement slot for Legendary: it can't be upgraded further.")]
    [InlineData(UpgradeStatus.NotAllowed, "This station doesn't upgrade Iron Sword to Legendary.")]
    [InlineData(UpgradeStatus.CantPay, "Not enough to upgrade Iron Sword to Legendary: 3 of 10 Metal Scraps")]
    [InlineData(UpgradeStatus.Canceled, "The upgrade of Iron Sword was stopped.")]
    [InlineData(UpgradeStatus.Stale, "That item is gone.")]
    [InlineData(UpgradeStatus.Refused, "The upgrade of Iron Sword did not happen: 3 of 10 Metal Scraps")]
    public void A_refusal_says_why(UpgradeStatus status, string text)
    {
        var from = status == UpgradeStatus.AtMax ? ItemRarity.Legendary : ItemRarity.Epic;
        Assert.Equal(text, UpgradeStations.Describe(Result(status, from, ItemRarity.Legendary, Gear(from), Gear(from), message: "3 of 10 Metal Scraps"), Name));
    }
}
