// SPDX-License-Identifier: MIT
// The kit's screen and window placement (KilnHarness.UiPlacement, GitHub issue 146, since 0.2.0-dev.31.11): Kiln.Ui's own
// placement code on frames the kit plays. A mod opens its screen beside another mod's, a call on a closed screen waits
// for its next open, the remembered placement survives a game start and is clamped into a smaller display, a corrupt
// file starts from the options, and the coverage table names it.
using Xunit;

namespace Kiln.TestKit.Tests;

[Collection("UiPlacementKit")]
public sealed class UiPlacementKitTests
{
    private sealed class PlacementMod : Mod;

    [Fact]
    public void A_mod_opens_its_screen_beside_another_mod_s()
    {
        using var kiln = new KilnHarness();
        var spells = kiln.Load<PlacementMod>(new ModManifest("betterspells-kit"));
        kiln.Step();
        var bs = kiln.As(spells, () => Ui.Screen("Better Spells", _ => { }, new UiScreenOptions { Position = (0.3f, 0.5f), Width = 600, Height = 500 }));
        var grimoire = kiln.Run(() => Ui.Screen("Grimoire", _ => { }, new UiScreenOptions { Width = 500, Height = 500 }));
        bs.Open();
        var b = kiln.UiPlacement.Frame(bs)!.Value;
        Assert.Equal(0.3f, b.X, 4);
        Assert.Equal(600f, b.Width);
        // Grimoire's left edge on Better Spells' right edge, at the same height
        grimoire.MoveTo(b.Right, b.Y, pivotX: 0);
        grimoire.Open();
        var g = kiln.UiPlacement.Frame(grimoire)!.Value;
        Assert.Equal(b.Right, g.Left, 4);
        Assert.Equal(b.Y, g.Y, 4);
        Assert.Equal(g, kiln.UiPlacement.Frame(grimoire));  // the move applied once; ImGui keeps it
        bs.Dispose();
        grimoire.Dispose();
    }

    [Fact]
    public void Calls_on_a_closed_screen_wait_for_its_next_open()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var s = kiln.Run(() => Ui.Screen("Waits", _ => { }));
        s.MoveTo(0.25f, 0.25f);
        s.Resize(400, 300);
        Assert.Null(kiln.UiPlacement.Frame(s));
        Assert.Null(s.Placement);
        s.Open();
        var p = kiln.UiPlacement.Frame(s)!.Value;
        Assert.Equal((0.25f, 0.25f, 400f, 300f), (p.X, p.Y, p.Width, p.Height));
        s.Close();
        s.Open();
        p = kiln.UiPlacement.Frame(s)!.Value;  // the next open starts from the options again: centred at 720 x 520
        Assert.Equal((0.5f, 0.5f, 720f, 520f), (p.X, p.Y, p.Width, p.Height));
        s.Dispose();
    }

    [Fact]
    public void The_player_s_placement_is_remembered_across_a_game_start_and_clamped_into_a_smaller_display()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<PlacementMod>(new ModManifest("grimoire-kit"));
        kiln.Step();
        var o = new UiScreenOptions { Resizable = true, RememberPlacement = true, PlacementId = "main" };
        var s = kiln.As(mod, () => Ui.Screen("Grimoire", _ => { }, o));
        s.Open();
        kiln.UiPlacement.Frame(s);
        kiln.UiPlacement.PlayerPlaces(s, 0.85f, 0.5f, 900, 600);
        kiln.UiPlacement.Frame(s);
        var kept = kiln.UiPlacement.Remembered(s)!.Value;
        Assert.Equal((0.85f, 0.5f, 900f, 600f), (kept.X, kept.Y, kept.Width, kept.Height));
        Assert.Equal(Path.Combine(kiln.ConfigDirectory, "ui-placement.json"), kiln.UiPlacement.FilePath);
        Assert.Contains("\"grimoire-kit\"", File.ReadAllText(kiln.UiPlacement.FilePath!));
        s.Dispose();

        // a game start on a smaller display: the screen opens where the player left it, inside the display
        kiln.UiPlacement.Restart();
        kiln.UiPlacement.ViewportWidth = 1280;
        kiln.UiPlacement.ViewportHeight = 720;
        var again = kiln.As(mod, () => Ui.Screen("Grimoire", _ => { }, o));
        again.Open();
        var p = kiln.UiPlacement.Frame(again)!.Value;
        Assert.Equal((900f, 600f), (p.Width, p.Height));
        Assert.Equal(1f, p.Right, 4);  // 0.85 of 1280 would put its right edge past the display
        Assert.True(p.Top >= 0 && p.Bottom <= 1);
        // an explicit call wins over the remembered placement
        again.MoveTo(0.5f, 0.5f);
        p = kiln.UiPlacement.Frame(again)!.Value;
        Assert.Equal((0.5f, 0.5f), (p.X, p.Y));
        again.Dispose();
    }

    [Fact]
    public void A_corrupt_file_starts_from_the_options()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        kiln.UiPlacement.Restart();
        File.WriteAllText(Path.Combine(kiln.ConfigDirectory, "ui-placement.json"), "{ not json");
        var w = kiln.Run(() => Ui.Window("Spell list", _ => { }, new UiWindowOptions { Width = 300, Height = 200, Position = (0.25f, 0.5f), RememberPlacement = true }));
        var p = kiln.UiPlacement.Frame(w)!.Value;
        Assert.Equal((0.25f, 0.5f, 300f, 200f), (p.X, p.Y, p.Width, p.Height));
        Assert.True(File.Exists(Path.Combine(kiln.ConfigDirectory, "ui-placement.json.bad")));
        Assert.NotNull(kiln.UiPlacement.Remembered(w));  // the frame wrote a good file again
        w.Dispose();
    }

    [Fact]
    public void A_window_sized_by_its_contents_is_moved_once_and_keeps_its_options_otherwise()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var w = kiln.Run(() => Ui.Window("Tools", _ => { }, new UiWindowOptions { AutoResize = true }));
        var first = kiln.UiPlacement.Frame(w)!.Value;
        Assert.Equal((320f, 240f), (first.Width, first.Height));
        w.MoveTo(1, 1, pivotX: 1, pivotY: 1);  // the bottom right corner
        var p = kiln.UiPlacement.Frame(w)!.Value;
        Assert.Equal((1f, 1f), (p.Right, p.Bottom));
        w.Hide();
        Assert.Null(kiln.UiPlacement.Frame(w));
        Assert.Equal(p, w.Placement);  // kept while hidden
        w.Dispose();
    }

    [Fact]
    public void The_coverage_table_names_the_placement_row()
    {
        var row = Assert.Single(Coverage.All, r => r.Area.StartsWith("Screen and window placement", StringComparison.Ordinal));
        Assert.Equal(CoverageKind.Real, row.Kind);
        Assert.Empty(row.Capabilities);  // kiln.ui keeps its own offline row
        Assert.Contains(Coverage.All, r => r.Area == "kiln.ui" && r.Kind == CoverageKind.Unavailable);
    }
}
