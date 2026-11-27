// SPDX-License-Identifier: MIT
// The generated config page with a soft range side (kiln#155, [Range(0, 1, 0.1, SoftMax = true)]): the slider spans
// min..max, the number field beside it takes and shows a value past the soft side, the hard side is refused as before.
using Kiln.FakeUi;
using Xunit;

namespace Kiln.ConfigScreensTests;

public sealed class SoftPageConfig
{
    [Range(0, 1, 0.1, SoftMax = true)]
    public double CostMultiplier { get; set; } = 1;

    [Range(0, 10, SoftMin = true)]
    public int Offset { get; set; } = 2;
}

[Collection(UiSerial.Name)]
public sealed class ConfigScreensSoftRangeTests : IDisposable
{
    private const string Mod = "softpagemod";
    private readonly IKilnBackend _previous = Backend.Current;
    private readonly string _previousRoot = KilnPaths.Root;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-softpage-" + Guid.NewGuid().ToString("N"));
    private readonly UiDraw _ui = new();
    private readonly ModContext _mod;
    private readonly ConfigFile _file;

    public ConfigScreensSoftRangeTests()
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
        try { Config.Register<SoftPageConfig>(); }
        finally { HostAccess.Current = previous; }
        _file = Config.All.Single(f => f.ModId == Mod);
        FakeUiTable.OpenTables = true;
        FakeUiTable.FieldLabel = "##n";
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

    // one frame of the page; -> what the two sliders were given (costMultiplier, offset)
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
    public void A_value_past_the_soft_side_is_typed_shown_and_saved_and_the_hard_side_is_refused()
    {
        Assert.Equal([1, 2], Draw());
        FakeUiTable.Type = t => t == "1" ? "3" : null;              // typed into the cost multiplier's field: past max 1
        Draw();
        FakeUiTable.Type = null;
        Assert.Equal([3, 2], Draw());                               // the slider is given the real value (ImGui puts its grab at the end)
        Assert.Equal(["3", "2"], FakeUiTable.Fields.Select(f => f.Text));
        Assert.True(Says("* costMultiplier"));
        Assert.False(Says("a number"));
        FakeUiTable.Click = "Apply";
        Draw();
        ConfigScreens.RunCommits();
        Assert.Equal(3.0, _file.Get("costMultiplier"));
        Assert.Equal([3, 2], Draw());                               // still 3 after the save: nothing clamped it
        Assert.Equal(["3", "2"], FakeUiTable.Fields.Select(f => f.Text));

        // below the hard min: not staged, the text stays while the field is in use, with what the field takes
        FakeUiTable.Type = t => t == "3" ? "-1" : null;
        FakeUiTable.Item = new UiNative.Item { Active = 1 };
        Draw();
        FakeUiTable.Type = null;
        Assert.Equal([3, 2], Draw());
        Assert.Equal(["-1", "2"], FakeUiTable.Fields.Select(f => f.Text));
        Assert.True(Says("a number of 0 or more"));
        FakeUiTable.Item = default;

        // the soft min side, whole numbers
        Draw();
        FakeUiTable.Type = t => t == "2" ? "-40" : null;
        Draw();
        FakeUiTable.Type = null;
        Assert.Equal([3, -40], Draw());
        FakeUiTable.Type = t => t == "-40" ? "11" : null;
        FakeUiTable.Item = new UiNative.Item { Active = 1 };
        Draw();
        FakeUiTable.Type = null;
        Assert.Equal([3, -40], Draw());
        Assert.Equal(["3", "11"], FakeUiTable.Fields.Select(f => f.Text));
        Assert.True(Says("a number of 10 or less"));
        FakeUiTable.Item = default;
    }

    [Fact]
    public void Dragging_the_slider_still_gives_a_value_of_its_span()
    {
        _file.Set("costMultiplier", 3.0);
        Assert.Equal([3, 2], Draw());
        FakeUiTable.Set = (0, 0.42);                                // dragged: snaps to the 0.1 grid inside 0..1
        Draw();
        Assert.Equal(0.4, Draw()[0], 6);
        Assert.Equal("0.4", FakeUiTable.Fields[0].Text);
    }

    [Theory]
    [InlineData("float", 0.0, 1.0, false, true, "3", true)]
    [InlineData("float", 0.0, 1.0, false, true, "1e6", true)]
    [InlineData("float", 0.0, 1.0, false, true, "-0.5", false)]
    [InlineData("float", 0.0, 1.0, true, false, "-0.5", true)]
    [InlineData("float", 0.0, 1.0, true, false, "1.5", false)]
    [InlineData("int", 0.0, 10.0, true, false, "-5", true)]
    [InlineData("int", 0.0, 10.0, true, true, "500", true)]
    [InlineData("int", 0.0, 10.0, true, true, "3000000000", false)]   // still an int
    [InlineData("float", 0.0, 1.0, false, false, "3", false)]           // no soft side: as before
    public void A_typed_number_past_a_soft_side_is_a_value(string type, double min, double max, bool softMin, bool softMax,
        string text, bool ok)
    {
        var e = new ConfigEntry("a", type, null, 0, 0, 0, min, max, [], false, false) { SoftMin = softMin, SoftMax = softMax };
        Assert.Equal(ok, ConfigScreens.TryNumber(e, text, out _));
    }

    [Fact]
    public void The_tooltip_says_where_the_slider_stops_and_that_more_can_be_typed()
    {
        var cost = _file.Entries.Single(e => e.Path == "costMultiplier");
        var text = ConfigScreens.Describe(cost);
        Assert.Contains("range 0..1", text);
        Assert.Contains("The slider stops at 1; a higher value can be typed.", text);
        Assert.DoesNotContain("lower value", text);
        var offset = ConfigScreens.Describe(_file.Entries.Single(e => e.Path == "offset"));
        Assert.Contains("The slider starts at 0; a lower value can be typed.", offset);
    }
}
