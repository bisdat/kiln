// SPDX-License-Identifier: MIT
// The kit's window docking (KilnHarness.UiPlacement.FrameWindows / PlayerDrags, GitHub issue 149): Kiln.Ui's own docking
// code on frames the kit plays. Two mods' windows dock and move together whichever one the player drags, Shift drags one
// alone and a drop beside it docks it again, a docked window opened by the same key press waits for its target, and the
// coverage table names it.
using Xunit;

namespace Kiln.TestKit.Tests;

[Collection("UiPlacementKit")]
public sealed class UiDockKitTests
{
    private sealed class DockMod : Mod;

    private static (UiWindow Spells, UiWindow Book) Pair(KilnHarness kiln, bool open = true)
    {
        var spells = kiln.Load<DockMod>(new ModManifest("betterspells-dock"));
        var grimoire = kiln.Load<DockMod>(new ModManifest("grimoire-dock"));
        kiln.Step();
        var bs = kiln.As(spells, () => Ui.Window("Better Spells", _ => { },
            new UiWindowOptions { Width = 300, Height = 400, Position = (0.3f, 0.5f), AcceptDock = true, PlacementId = "spells", StartOpen = open }));
        var book = kiln.As(grimoire, () => Ui.Window("Grimoire", _ => { }, new UiWindowOptions { Width = 250, Height = 300, StartOpen = open }));
        return (bs, book);
    }

    [Fact]
    public void Two_mods_windows_dock_and_move_together_whichever_one_the_player_drags()
    {
        using var kiln = new KilnHarness();
        var (bs, book) = Pair(kiln);
        var target = Ui.FindWindow("betterspells-dock", "spells");
        Assert.NotNull(target);
        book.DockTo(target!, DockSide.Right);
        var f = kiln.UiPlacement.FrameWindows();
        Assert.Equal(f[bs].Right, f[book].Left, 4);
        Assert.Equal(f[bs].Top, f[book].Top, 4);
        Assert.Equal(0.3f, f[bs].X, 4);

        kiln.UiPlacement.PlayerDrags(bs, 0.5f, 0.5f);  // Better Spells: Grimoire follows in the same frame
        f = kiln.UiPlacement.FrameWindows();
        Assert.Equal(0.5f, f[bs].X, 4);
        Assert.Equal(f[bs].Right, f[book].Left, 4);

        kiln.UiPlacement.PlayerDrags(book, 0.7f, 0.4f);  // Grimoire: Better Spells moves with it
        f = kiln.UiPlacement.FrameWindows();
        Assert.Equal(0.7f, f[book].X, 4);
        Assert.Equal(0.4f, f[book].Y, 4);
        Assert.Equal(f[bs].Right, f[book].Left, 4);
        f = kiln.UiPlacement.FrameWindows();  // the drop: still docked
        Assert.Equal(target, book.DockedTo);
        Assert.Equal(f[bs].Right, f[book].Left, 4);

        bs.MoveTo(0.2f, 0.5f);  // a mod's move: the same
        f = kiln.UiPlacement.FrameWindows();
        Assert.Equal(0.2f, f[bs].X, 4);
        Assert.Equal(f[bs].Right, f[book].Left, 4);
    }

    [Fact]
    public void Shift_drags_one_alone_and_a_drop_beside_its_old_side_docks_it_again()
    {
        using var kiln = new KilnHarness();
        var (bs, book) = Pair(kiln);
        book.DockTo(Ui.FindWindow("betterspells-dock", "spells")!, DockSide.Right);
        var changes = new List<UiDockChange>();
        book.DockChanged += changes.Add;
        var f = kiln.UiPlacement.FrameWindows();
        var spells = f[bs];

        kiln.UiPlacement.PlayerDrags(book, 0.8f, 0.8f, shift: true);
        f = kiln.UiPlacement.FrameWindows();
        Assert.Null(book.DockedTo);
        Assert.Equal(spells, f[bs]);  // stayed
        Assert.True(Assert.Single(changes).ByPlayer);
        kiln.UiPlacement.FrameWindows();

        // dropped with its left edge 10 px right of Better Spells' right edge, 50 px lower: docked again there
        var x = (spells.Right * 1920 + 10 + 125) / 1920;
        var y = (spells.Top * 1080 + 50 + 150) / 1080;
        kiln.UiPlacement.PlayerDrags(book, x, y);
        kiln.UiPlacement.FrameWindows();
        f = kiln.UiPlacement.FrameWindows();
        Assert.NotNull(book.DockedTo);
        Assert.Equal(2, changes.Count);
        Assert.True(changes[1].ByPlayer);
        Assert.Equal(50, changes[1].Offset, 2);
        f = kiln.UiPlacement.FrameWindows();
        Assert.Equal(f[bs].Right, f[book].Left, 4);
        Assert.Equal(f[bs].Top + 50f / 1080, f[book].Top, 4);
    }

    [Fact]
    public void A_docked_window_opened_by_the_same_key_press_waits_for_its_target()
    {
        using var kiln = new KilnHarness();
        var (bs, book) = Pair(kiln, open: false);
        book.DockTo(Ui.FindWindow("betterspells-dock", "spells")!, DockSide.Left);
        book.Show();  // Grimoire's binding ran first
        var f = kiln.UiPlacement.FrameWindows();
        Assert.False(f.ContainsKey(book));  // not drawn at its own place
        bs.Show();
        f = kiln.UiPlacement.FrameWindows();
        Assert.Equal(f[bs].Left, f[book].Right, 4);
    }

    [Fact]
    public void The_coverage_table_names_the_docking()
    {
        var row = Assert.Single(Coverage.All, r => r.Area.StartsWith("Screen and window placement", StringComparison.Ordinal));
        Assert.Contains("FrameWindows", row.Note);
        Assert.Contains("DockTo", row.Note);
    }
}
