// SPDX-License-Identifier: MIT
// The Kiln menu's generated config page (ConfigScreens): what the table shows around "Apply", and the number field
// beside a slider (GitHub issue 28). No game: a fake kiln.ui table, a config registered by a test mod, and the tick's
// save called by hand.
using Kiln.FakeUi;
using Xunit;

namespace Kiln.ConfigScreensTests;

public sealed class PageConfig
{
    [Range(0.01, 1000, 0.1)]
    public double Spread { get; set; } = 1;

    [Range(0, 100)]
    public int Share { get; set; } = 10;

    public bool Sort { get; set; } = true;
}

[Collection(UiSerial.Name)]
public sealed class ConfigScreensTests : IDisposable
{
    private const string Mod = "pagemod";
    private readonly IKilnBackend _previous = Backend.Current;
    private readonly string _previousRoot = KilnPaths.Root;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-configpage-" + Guid.NewGuid().ToString("N"));
    private readonly UiDraw _ui = new();
    private readonly ModContext _mod;
    private readonly ConfigFile _file;

    public ConfigScreensTests()
    {
        FakeUiTable.Install();
        Backend.Current = new FakeUiTable.Backend();
        UiNative.ResetForTests();
        Directory.CreateDirectory(Path.Combine(_root, "config"));
        KilnPaths.Root = _root;
        KilnPaths.Config = null!;
        Config.Reset();
        ConfigScreens.ResetForTests();
        _mod = HostAccess.CreateContext(new ModInfo(Mod, Mod, "1.0.0", _root), 0, _root);
        var previous = HostAccess.Current;
        HostAccess.Current = _mod;
        try { Config.Register<PageConfig>(); }
        finally { HostAccess.Current = previous; }
        _file = Config.All.Single(f => f.ModId == Mod);
        FakeUiTable.OpenTables = true;
    }

    public void Dispose()
    {
        ConfigScreens.ResetForTests();
        _mod.Retire();
        Config.Reset();
        KilnPaths.Root = _previousRoot;
        KilnPaths.Config = null!;
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
        try { Directory.Delete(_root, true); }
        catch (IOException) { }
    }

    // one frame of the page; -> what the three value widgets were given (spread, share, sort)
    private double[] Draw()
    {
        FakeUiTable.Values.Clear();
        lock (FakeUiTable.Texts) FakeUiTable.Texts.Clear();
        FakeUiTable.Fields.Clear();
        ConfigScreens.Draw(_ui, Mod);
        return [.. FakeUiTable.Values];
    }

    private static bool Says(string part)
    {
        lock (FakeUiTable.Texts) return FakeUiTable.Texts.Any(t => t.Contains(part, StringComparison.Ordinal));
    }

    [Fact]
    public void After_Apply_the_table_shows_the_new_values_before_the_tick_saved_them_and_after()
    {
        Assert.Equal([1, 10, 1], Draw());
        FakeUiTable.Set = (1, 50);                      // the share slider is dragged to 50
        Draw();
        FakeUiTable.Set = (2, 0);                       // the sort checkbox is cleared
        Assert.Equal([1, 50, 1], Draw());
        Assert.True(Says("* share"));
        FakeUiTable.Click = "Apply";
        Assert.Equal([1, 50, 0], Draw());
        Assert.True(Says("* share") && Says("* sort"));    // both were marked as changed when Apply was clicked

        // no tick has come (the game is paused in the pause menu): the entries still hold the old values, the table shows
        // what was applied. Before, it showed the old values again until the page was opened the next time
        Assert.Equal(10, _file.Get("share"));
        Assert.Equal([1, 50, 0], Draw());
        Assert.False(Says("* share") || Says("* sort"));
        Assert.True(Says("2 changes applied"));

        ConfigScreens.RunCommits();                     // the tick
        Assert.Equal(50, _file.Get("share"));
        Assert.Equal(false, _file.Get("sort"));
        Assert.Equal([1, 50, 0], Draw());
        Assert.False(Says("applied"));
    }

    [Fact]
    public void A_refused_value_comes_back_to_the_page_with_its_reason_and_a_newer_edit_is_not_overwritten()
    {
        ConfigScreens.Stage(_file, "share", 500);        // outside 0 to 100: the config refuses it
        FakeUiTable.Click = "Apply";
        Draw();
        ConfigScreens.RunCommits();
        Assert.Equal(10, _file.Get("share"));
        Assert.Equal([1, 500, 1], Draw());              // still on the page, to fix
        Assert.True(Says("* share"));

        // the player changes the entry again while an Apply of it waits for the tick: the tick's refusal leaves that edit
        ConfigScreens.Stage(_file, "share", 700);
        FakeUiTable.Click = "Apply";
        Draw();
        ConfigScreens.Stage(_file, "share", 40);
        ConfigScreens.RunCommits();
        Assert.Equal([1, 40, 1], Draw());
        FakeUiTable.Click = "Apply";
        Draw();
        ConfigScreens.RunCommits();
        Assert.Equal(40, _file.Get("share"));
        Assert.Equal([1, 40, 1], Draw());
    }

    [Fact]
    public void A_refused_Apply_does_not_come_back_over_a_newer_Apply_that_waits()
    {
        // no tick between the two: the game is paused
        ConfigScreens.Stage(_file, "share", 500);        // refused when it is saved
        FakeUiTable.Click = "Apply";
        Draw();
        ConfigScreens.Stage(_file, "share", 40);
        FakeUiTable.Click = "Apply";
        Draw();
        Assert.Equal([1, 40, 1], Draw());
        ConfigScreens.RunCommits();                     // the tick saves both, in order
        Assert.Equal(40, _file.Get("share"));
        Assert.Equal([1, 40, 1], Draw());
        Assert.False(Says("* share"));                  // the refused 500 is not back on the page
    }

    /// <summary>A write that failed elsewhere than the page's own Apply (the mod's Set, a Save: neither throws for it,
    /// ConfigFile.LastWriteError says why) shows on the mod's page with the file's other errors, until a write succeeds.</summary>
    [Fact]
    public void A_write_that_failed_outside_the_page_shows_on_it_until_a_write_succeeds()
    {
        Draw();
        Assert.False(Says("cannot write"));
        var file = _file.FilePath;
        if (File.Exists(file)) File.Delete(file);
        Directory.CreateDirectory(file);  // the file's place is taken: every write fails
        try
        {
            _file.Set("share", 20);  // the mod's own Set: the value applies for this session, the file does not hold it
            Assert.Equal(20, _file.Get("share"));
            Draw();
            Assert.True(Says("not saved: cannot write " + file));
            _file.Save();
            Draw();
            Assert.True(Says("not saved: cannot write"));
        }
        finally
        {
            Directory.Delete(file, true);
        }
        _file.Save();  // writable again: the file holds every value
        Assert.Null(_file.LastWriteError);
        Draw();
        Assert.False(Says("cannot write"));
    }

    [Fact]
    public void The_default_buttons_see_the_applied_value_that_waits()
    {
        FakeUiTable.Set = (1, 50);
        Draw();
        FakeUiTable.Click = "Apply";
        Draw();
        Assert.Equal([1, 50, 1], Draw());               // applied, not saved: the entry itself still holds its default
        Assert.True(Says("default"));                   // the row offers its default
        FakeUiTable.Click = "All to defaults";
        Draw();
        Assert.Equal([1, 10, 1], Draw());               // staged back to the default
        FakeUiTable.Click = "Apply";
        Draw();
        ConfigScreens.RunCommits();
        Assert.Equal(10, _file.Get("share"));
        Assert.Equal([1, 10, 1], Draw());
    }

    [Fact]
    public void A_value_is_typed_into_the_number_field_beside_a_slider()
    {
        FakeUiTable.FieldLabel = "##n";
        Draw();
        Assert.Equal(["1", "10"], FakeUiTable.Fields.Select(f => f.Text));   // one field per slider, none for the checkbox

        FakeUiTable.Type = t => t == "1" ? "2,5" : null;                     // typed into spread's field
        Draw();
        FakeUiTable.Type = null;
        Assert.Equal([2.5, 10, 1], Draw());
        Assert.Equal(["2.5", "10"], FakeUiTable.Fields.Select(f => f.Text));
        Assert.True(Says("* spread"));

        // half typed, or outside the range: nothing is staged, the text stays while the field is in use, with the range
        FakeUiTable.Type = t => t == "2.5" ? "5000" : null;
        FakeUiTable.Item = new UiNative.Item { Active = 1 };
        Draw();
        FakeUiTable.Type = null;
        Assert.Equal([2.5, 10, 1], Draw());
        Assert.Equal(["5000", "10"], FakeUiTable.Fields.Select(f => f.Text));
        Assert.True(Says("a number from 0.01 to 1000"));
        FakeUiTable.Item = default;                                          // the field is left
        Draw();
        Draw();
        Assert.Equal(["2.5", "10"], FakeUiTable.Fields.Select(f => f.Text));
        Assert.False(Says("a number from"));

        FakeUiTable.Click = "Apply";
        Draw();
        ConfigScreens.RunCommits();
        Assert.Equal(2.5, _file.Get("spread"));
    }

    [Fact]
    public void Only_slider_interaction_snaps_an_off_grid_value_before_Apply()
    {
        FakeUiTable.FieldLabel = "##n";
        Draw();
        Assert.Equal("1", FakeUiTable.Fields[0].Text); // off-grid saved/default value stays exact
        Assert.False(Says("* spread"));
        FakeUiTable.Set = (0, 0.348);                 // drag the float slider: min 0.01 + 3 * 0.1
        Draw();
        Draw();
        Assert.Equal("0.31", FakeUiTable.Fields[0].Text);
        Assert.Equal(1.0, _file.Get("spread"));
        FakeUiTable.Click = "Apply";
        Draw();
        ConfigScreens.RunCommits();
        Assert.Equal(0.31, _file.Get("spread"));
    }

    [Theory]
    [InlineData("float", -0.35, 1.05, 0.1, -0.18, -0.15)]
    [InlineData("float", 0.05, 1.0, 0.1, 0.348, 0.35)]
    [InlineData("float", 0.0, 1.0, 0.00001, 0.123456, 0.12346)]
    [InlineData("float", 0.05, 1.0, 0.1, 1.0, 0.95)] // max is not a grid point
    [InlineData("int", -3.0, 20.0, 4.0, 4.0, 5.0)]
    [InlineData("int", 3.0, 20.0, 4.0, 20.0, 19.0)]
    [InlineData("float", 0.0, 1.0, 0.0, 0.123456, 0.1235)]
    [InlineData("float", 0.0, 1.0, double.NaN, 0.123456, 0.1235)]
    [InlineData("int", 0.0, 20.0, 0.5, 3.0, 3.0)]
    public void Slider_values_use_a_min_anchored_decimal_grid_or_the_legacy_fallback(
        string type, double min, double max, double step, double value, double expected)
    {
        var entry = new ConfigEntry("a", type, null, value, value, value, min, max, [], false, false) { Step = step };
        Assert.Equal(expected, ConfigScreens.SliderValue(entry, value));
        Assert.True(ConfigScreens.TryNumber(entry, "0.123456", out var typed) || type == "int" || min > 0.123456);
        if (type == "float" && min <= 0.123456) Assert.Equal(0.123456, typed);
    }

    [Theory]
    [InlineData("float", "2.5", 2.5)]
    [InlineData("float", " 250 ", 250.0)]
    [InlineData("float", "2,5", 2.5)]
    [InlineData("float", "1e2", 100.0)]
    [InlineData("int", "42", 42)]
    public void A_typed_number_of_the_range_is_a_value(string type, string text, object expected)
    {
        var e = new ConfigEntry("a", type, null, 1, 1, 1, type == "int" ? 0 : 0.01, 1000, [], false, false);
        Assert.True(ConfigScreens.TryNumber(e, text, out var value));
        Assert.Equal(expected, value);
    }

    [Theory]
    [InlineData(2.5, "2.5")]
    [InlineData(250.0, "250")]
    [InlineData(0.1, "0.1")]
    [InlineData((double)0.1f, "0.1")]                 // a single-precision setting's value, not 0.10000000149011612
    [InlineData(1234.5678, "1234.5678")]
    [InlineData(0.01, "0.01")]
    public void The_number_field_shows_the_shortest_text_of_the_value(double value, string text)
    {
        Assert.Equal(text, ConfigScreens.NumberText(value));
        Assert.True(ConfigScreens.TryNumber(new ConfigEntry("a", "float", null, 1, 1, 1, 0.01, 10000, [], false, false), text, out var back));
        Assert.Equal(value, (double)back, 6);            // and typing that text gives the value again
    }

    [Theory]
    [InlineData("float", "")]
    [InlineData("float", "-")]
    [InlineData("float", "abc")]
    [InlineData("float", "0")]        // below 0.01
    [InlineData("float", "1000.5")]
    [InlineData("float", "NaN")]
    [InlineData("int", "2.5")]
    [InlineData("int", "1001")]
    [InlineData("int", "99999999999")]
    public void A_text_that_is_no_number_of_the_range_is_no_value(string type, string text)
    {
        var e = new ConfigEntry("a", type, null, 1, 1, 1, type == "int" ? 0 : 0.01, 1000, [], false, false);
        Assert.False(ConfigScreens.TryNumber(e, text, out _));
    }
}
