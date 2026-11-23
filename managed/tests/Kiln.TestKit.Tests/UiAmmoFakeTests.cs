// SPDX-License-Identifier: MIT
// The kit's fake kiln.ui.ammo (Fakes/UiAmmo/): Ui.AmmoList's own code on it. The game's side (Hold, SetGameRows, NewSession,
// KnownItems), what the HUD shows (Shown, Provided, ModeOf), the selection kept by item, picks of rows the game cannot
// equip itself, and the rows a test switches off.
using Xunit;

namespace Kiln.TestKit.Tests;

[Collection("UiAmmoFake")]
public sealed unsafe class UiAmmoFakeTests
{
    private static readonly ItemId Bow = new(0xB0), Staff = new(0x5A);
    private static AmmoListRow R(uint item, int count) => new(new ItemId(item), count);

    [Fact]
    public void TheTableIsTheHeadersV1()
    {
        var t = (uint*)KitBackend.Instance.Service("kiln.ui.ammo", 1);  // KilnServiceHeader { size, version }, three functions
        Assert.True(t != null);
        Assert.Equal((32u, 1u), (t[0], t[1]));
    }

    [Fact]
    public void ProvidedRowsFollowTheGamesAndAPickOfOneNotInTheBackpackReachesPicked()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var picks = new List<AmmoListPick>();
        kiln.AmmoList.Hold(AmmoListKind.Arrow, Bow, R(0x10, 20));
        using var p = kiln.Run(() => Ui.AmmoList.Provide(AmmoListKind.Arrow, c => [R(0x30, 12), R(0x10, 5)]));
        kiln.Run(() => Ui.AmmoList.Picked += picks.Add);
        Assert.Equal([R(0x10, 20)], kiln.AmmoList.Shown(AmmoListKind.Arrow));  // nothing until the next UI frame
        kiln.Step();
        Assert.Equal([R(0x10, 20), R(0x30, 12)], kiln.AmmoList.Shown(AmmoListKind.Arrow));  // the game's own 0x10 stays
        Assert.Equal([R(0x30, 12)], kiln.AmmoList.Provided(AmmoListKind.Arrow));
        Assert.Equal(AmmoListMode.Append, kiln.AmmoList.ModeOf(AmmoListKind.Arrow));
        Assert.False(kiln.AmmoList.Pick(AmmoListKind.Arrow, new ItemId(0x10)));  // the game equips its own row
        Assert.True(kiln.AmmoList.Pick(AmmoListKind.Arrow, new ItemId(0x30)));
        Assert.Empty(picks);
        kiln.Step();
        Assert.Equal([new AmmoListPick(AmmoListKind.Arrow, new ItemId(0x30), 12)], picks);
    }

    [Fact]
    public void ReplaceRowsAreBuiltAgainWhenTheHeldWeaponOrTheBackpackChanges()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var seen = new List<(ItemId Held, int GameRows)>();
        kiln.AmmoList.Player = new EntityId(77);
        kiln.AmmoList.Hold(AmmoListKind.Spell, Staff, R(0x40, 1));
        using var p = kiln.Run(() => Ui.AmmoList.Provide(AmmoListKind.Spell, c =>
        {
            seen.Add((c.Held, c.GameRows.Count));
            Assert.Equal(new EntityId(77), c.Player);
            return [R(0x41, 1), .. c.GameRows];
        }, new AmmoListOptions { Mode = AmmoListMode.Replace }));
        kiln.Step(3);
        Assert.Equal([R(0x41, 1), R(0x40, 1)], kiln.AmmoList.Shown(AmmoListKind.Spell));
        kiln.AmmoList.SetGameRows(AmmoListKind.Spell, R(0x40, 1), R(0x42, 1));
        kiln.Step();
        kiln.AmmoList.Hold(AmmoListKind.Spell, new ItemId(0x5B));
        kiln.Step();
        Assert.Equal([(Staff, 1), (Staff, 2), (new ItemId(0x5B), 0)], seen);
        Assert.Equal([R(0x41, 1)], kiln.AmmoList.Shown(AmmoListKind.Spell));
        Assert.Equal(AmmoListMode.Replace, kiln.AmmoList.ModeOf(AmmoListKind.Spell));
        kiln.AmmoList.NewSession();
        Assert.Null(kiln.AmmoList.ModeOf(AmmoListKind.Spell));  // native clears at a world's stop
        kiln.Step();
        Assert.Equal(4, seen.Count);
        Assert.Equal([R(0x41, 1)], kiln.AmmoList.Shown(AmmoListKind.Spell));
    }

    [Fact]
    public void TheSelectionStaysOnItsItemWhenTheRowsChange()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var first = true;
        kiln.AmmoList.Hold(AmmoListKind.Arrow, Bow, R(0x10, 5));
        using var p = kiln.Run(() => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => first ? [R(0x30, 1), R(0x31, 1)] : [R(0x32, 1), R(0x31, 1)]));
        kiln.Step();
        kiln.AmmoList.Select(AmmoListKind.Arrow, new ItemId(0x31));
        first = false;
        p.Refresh();
        kiln.Step();
        Assert.Equal([R(0x10, 5), R(0x32, 1), R(0x31, 1)], kiln.AmmoList.Shown(AmmoListKind.Arrow));
        Assert.Equal(new ItemId(0x31), kiln.AmmoList.Selected(AmmoListKind.Arrow));
    }

    [Fact]
    public void RowsTheGameRefusesDropOnlyTheirProviderAndDisposingShowsTheGamesList()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        kiln.AmmoList.Hold(AmmoListKind.Arrow, Bow, R(0x10, 5));
        kiln.AmmoList.KnownItems(AmmoListKind.Arrow, new ItemId(0x10), new ItemId(0x30));
        var good = kiln.Run(() => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 2)]));
        var bad = kiln.Run(() => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x40, 1)]));  // a spell in the arrow list
        kiln.Step();
        Assert.Equal([R(0x30, 2)], kiln.AmmoList.Provided(AmmoListKind.Arrow));
        good.Dispose();
        bad.Dispose();
        kiln.Step();
        Assert.Null(kiln.AmmoList.ModeOf(AmmoListKind.Arrow));
        Assert.Equal([R(0x10, 5)], kiln.AmmoList.Shown(AmmoListKind.Arrow));
    }

    [Fact]
    public void WithThePickRowOffPickedRefusesAndWithTheListRowOffProvideRefuses()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        kiln.Capabilities.Override(Ui.AmmoList.PickCapability, false);
        var e = Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => Ui.AmmoList.Picked += _ => { }));
        Assert.Equal(Ui.AmmoList.PickCapability, e.Capability);
        Assert.False(kiln.Run(() => Ui.AmmoList.PicksAvailable));
        kiln.Capabilities.ClearOverride(Ui.AmmoList.PickCapability);
        kiln.Capabilities.Override(Ui.AmmoList.ListCapability, false);
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [])));
        Assert.False(kiln.Run(() => Ui.AmmoList.Available));
        kiln.Capabilities.ClearOverride(Ui.AmmoList.ListCapability);
        Assert.True(kiln.Run(() => Ui.AmmoList.Available));
    }
}
