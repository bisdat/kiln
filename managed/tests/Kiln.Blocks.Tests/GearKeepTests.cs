// SPDX-License-Identifier: MIT
// Kiln's keeping of one item's per-instance values through loads (the equipment wave, package P7; GearKeepRuntime): the
// pass that writes a kept value again wherever the item differs (after a load the game sets a level outside its type's
// range back and computes max durability again; a world restart forgets kiln.items' scale table), once per value per
// 50 steps, nothing for an item that is gone; the stored value's reading (bad values read as none) and the factor a Kiln
// client's tooltip takes from the synced value. What the game does after a load is the lab run of package P7.
using Xunit;

namespace Kiln.BlocksTests;

[Collection("gear keeper")]
public sealed class GearKeepTests : IDisposable
{
    public void Dispose() => GearKeepRuntime.ResetForTests();

    private static ItemInstanceRef Item(uint pide, uint stamp) =>
        new(new EntityRef(new EntityId(pide), IdentitySources.Stamp(EntitySourceOrder.Items, stamp)), new ItemId(0x1234));

    private static GearState State(ItemInstanceRef item, int level, int unlocked, int? durabilityMax) =>
        new(item, new ItemId(0x1234), ItemRarity.Rare, ItemRarity.Common, ItemRarity.Legendary, level, level - unlocked, 3, unlocked,
            level + 3 - unlocked, false, durabilityMax is null ? null : 50, durabilityMax, [])
        { TypeLevelMin = 5, TypeLevelMax = 10 };

    private sealed class Game : GearKeepRuntime.IGame
    {
        public readonly List<(ItemInstanceRef, GearKeptData)> Data = [];
        public readonly Dictionary<ItemInstanceRef, (double, double)> ScalesNow = [];
        public readonly Dictionary<ItemInstanceRef, GearState> Gears = [];
        public readonly List<string> Writes = [];

        public IReadOnlyList<(ItemInstanceRef Item, GearKeptData Data)> Kept() => Data;
        public GearState? Gear(ItemInstanceRef item) => Gears.GetValueOrDefault(item);
        public (double Power, double Armor)? Scales(ItemInstanceRef item) => ScalesNow.TryGetValue(item, out var s) ? s : null;

        public bool SetScale(ItemInstanceRef item, uint which, double scale)
        {
            Writes.Add($"{(which == 1 ? "power" : "armor")} {item.Entity.Id.Value} {scale}");
            return true;
        }

        public bool WriteLevel(ItemInstanceRef item, int level)
        {
            Writes.Add($"level {item.Entity.Id.Value} {level}");
            return true;
        }

        public bool WriteDurabilityMax(ItemInstanceRef item, int max)
        {
            Writes.Add($"durability {item.Entity.Id.Value} {max}");
            return true;
        }
    }

    [Fact]
    public void AfterALoadEveryKeptValueIsWrittenAgainOnce()
    {
        var game = new Game();
        var sword = Item(5000, 7);
        game.Data.Add((sword, new GearKeptData { Power = 1.5, Armor = 0.5, BaseLevel = 30, DurabilityMax = 900 }));
        game.ScalesNow[sword] = (1, 1);                    // a restart forgot the scale table
        game.Gears[sword] = State(sword, 11, 1, 300);       // the load set base 30 back to 10 and computed max 300 again
        Assert.Equal(4, GearKeepRuntime.Pass(game, 100));
        Assert.Equal(["power 5000 1.5", "armor 5000 0.5", "level 5000 31", "durability 5000 900"], game.Writes);  // base 30 + 1 unlocked
        game.Writes.Clear();
        Assert.Equal(0, GearKeepRuntime.Pass(game, 125));   // not asked again within 50 steps
        Assert.Equal(4, GearKeepRuntime.Pass(game, 150));   // still different 50 steps later: asked again
    }

    [Fact]
    public void AnItemAsKeptOrGoneIsLeftAlone()
    {
        var game = new Game();
        var kept = Item(5000, 7);
        var gone = Item(5001, 8);
        game.Data.Add((kept, new GearKeptData { Power = 2, BaseLevel = 30, DurabilityMax = 900 }));
        game.Data.Add((gone, new GearKeptData { Power = 2 }));
        game.ScalesNow[kept] = (2, 1);
        game.Gears[kept] = State(kept, 32, 2, 900);         // base 30, max 900: as kept
        Assert.Equal(0, GearKeepRuntime.Pass(game, 100));
        Assert.Empty(game.Writes);
    }

    [Fact]
    public void AStoredValueThatDoesNotReadIsNone()
    {
        Assert.True(GearKeepRuntime.ToKept(new GearKeptData { Power = 0, Armor = double.NaN, BaseLevel = 0, DurabilityMax = -3 }).IsEmpty);
        Assert.True(GearKeepRuntime.ToKept(new GearKeptData { Power = 1000, BaseLevel = 300, DurabilityMax = 2000000 }).IsEmpty);
        Assert.Equal(new GearKept(1.25, 1, 60, null), GearKeepRuntime.ToKept(new GearKeptData { Power = 1.25, BaseLevel = 60 }));
        var data = GearKeepRuntime.ToData(new GearKept(1.25, 1, 60, null), 1.25);
        Assert.Equal((1.25, (double?)null, 60, (int?)null, 1.25), (data.Power!.Value, data.Armor, data.BaseLevel!.Value, data.DurabilityMax, data.Shown!.Value));
        Assert.Null(GearKeepRuntime.ToData(new GearKept(1, 1, 60, null), 1).Shown);  // a factor of 1 is not sent
    }

    [Fact]
    public void AClientsTooltipFactorIsTheSyncedShownMultiplier()
    {
        Assert.Equal(1.5f, GearKeepRuntime.FactorOf("{\"Power\":1.5,\"Shown\":1.5}"));
        Assert.Equal(0.5f, GearKeepRuntime.FactorOf("{\"Armor\":0.5,\"Shown\":0.5}"));
        Assert.Equal(1f, GearKeepRuntime.FactorOf("{\"BaseLevel\":60}"));               // nothing to scale
        Assert.Equal(1f, GearKeepRuntime.FactorOf("{\"Shown\":5000}"));                 // outside 0.01..100: ignored
        Assert.Equal(1f, GearKeepRuntime.FactorOf("not json"));
    }
}
