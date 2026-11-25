// SPDX-License-Identifier: MIT
// Where a mod's screen or window sits (GitHub issue 146, since 0.2.0-dev.31.11, mod API 0.2.20): UiScreenOptions.Position,
// MoveTo / Resize applied once on the next frame (ImGuiCond_Always for that frame only), calls on a closed or removed screen,
// the remembered placement (ui-placement.json in the config folder: round trip, a corrupt or missing file, bad entries),
// clamping into the viewport and the order of precedence (an explicit call, then the remembered placement, then the
// options). No game: a fake kiln.ui table records the SetNextWindowPos / SetNextWindowSize calls and answers WindowRect.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.UiPlacementTests;

[Collection(UiSerial.Name)]
public sealed unsafe class UiPlacementTests : IDisposable
{
    private const uint Always = 1, FirstUseEver = 4, Appearing = 8;

    private static readonly List<(float X, float Y, uint Cond, float Px, float Py)> Pos = [];
    private static readonly List<(float W, float H, uint Cond)> Size = [];
    private static (float X, float Y, float W, float H) _rect;
    private static bool _visible = true;

    private readonly IKilnBackend _previous = Backend.Current;
    private readonly string _previousRoot = KilnPaths.Root;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-placement-" + Guid.NewGuid().ToString("N"));
    private readonly List<IDisposable> _made = [];
    private readonly FakeUiTable.Backend _backend = new();
    private readonly ModContext _mod;

    public UiPlacementTests()
    {
        FakeUiTable.Install();
        ref var t = ref FakeUiTable.Table->Base.Base.Base;
        t.SetNextWindowPos = &SetPos;
        t.SetNextWindowSize = &SetSize;
        t.Begin = &Begin;
        t.WindowRect = &GetRect;
        Backend.Current = _backend;
        UiNative.ResetForTests();
        Directory.CreateDirectory(Path.Combine(_root, "config"));
        KilnPaths.Root = _root;
        KilnPaths.Config = null!;
        UiPlacements.ResetForTests();
        UiPlacements.Debounce = TimeSpan.FromHours(1);  // the tests flush themselves
        _mod = HostAccess.CreateContext(new ModInfo("grimoire", "Grimoire", "1.0.0", _root), 0, _root);
        Viewport(1920, 1080, 1);
        Pos.Clear();
        Size.Clear();
        _rect = default;
        _visible = true;
    }

    public void Dispose()
    {
        foreach (var d in _made) d.Dispose();
        UiPlacements.ResetForTests();
        KilnPaths.Root = _previousRoot;
        KilnPaths.Config = null!;
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
        try { Directory.Delete(_root, true); }
        catch (IOException) { }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void SetPos(float x, float y, uint cond, float px, float py) => Pos.Add((x, y, cond, px, py));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void SetSize(float w, float h, uint cond) => Size.Add((w, h, cond));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte Begin(NativeStr title, byte* open, uint flags) => _visible ? (byte)1 : (byte)0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void GetRect(float* x, float* y, float* w, float* h)
    {
        *x = _rect.X;
        *y = _rect.Y;
        *w = _rect.W;
        *h = _rect.H;
    }

    private static void Viewport(float w, float h, float scale) =>
        UiEngine.DrawContext.Frame = new UiNative.Frame { Width = w, Height = h, DpiScale = scale };

    private UiScreen Screen(UiScreenOptions? o = null, string title = "Grimoire")
    {
        var s = UiEngine.AddScreen(new UiScreen(title, _ => { }, o ?? new UiScreenOptions { DimBackground = false }, _mod));
        _made.Add(s);
        return s;
    }

    private UiWindow Window(UiWindowOptions? o = null, string title = "Spell list")
    {
        var w = UiEngine.AddWindow(new UiWindow(title, _ => { }, o ?? new UiWindowOptions(), _mod));
        _made.Add(w);
        return w;
    }

    /// <summary>One overlay frame of the screens; what Kiln asked for (the last position and size call).</summary>
    private static ((float X, float Y, uint Cond, float Px, float Py)? Pos, (float W, float H, uint Cond)? Size) FrameScreens()
    {
        Pos.Clear();
        Size.Clear();
        UiEngine.DrawScreens(UiEngine.DrawContext);
        return (Pos.Count > 0 ? Pos[^1] : null, Size.Count > 0 ? Size[^1] : null);
    }

    private static ((float X, float Y, uint Cond, float Px, float Py)? Pos, (float W, float H, uint Cond)? Size) FrameWindow(UiWindow w)
    {
        Pos.Clear();
        Size.Clear();
        UiEngine.DrawWindow(UiEngine.DrawContext, w);
        return (Pos.Count > 0 ? Pos[^1] : null, Size.Count > 0 ? Size[^1] : null);
    }

    private string PlacementFile => Path.Combine(_root, "config", "ui-placement.json");

    // ---- options ----

    [Fact]
    public void The_new_options_are_off_by_default()
    {
        var s = new UiScreenOptions();
        Assert.Null(s.Position);
        Assert.False(s.RememberPlacement);
        Assert.Null(s.PlacementId);
        var w = new UiWindowOptions();
        Assert.False(w.RememberPlacement);
        Assert.Null(w.PlacementId);
    }

    [Fact]
    public void A_screen_without_the_new_options_is_centred_at_its_size_on_every_open_as_before()
    {
        var s = Screen();
        s.Open();
        var (pos, size) = FrameScreens();
        Assert.Equal((720f, 520f, Appearing), size);
        Assert.Equal((960f, 540f, Appearing, 0.5f, 0.5f), pos);
        Assert.Null(s.Placement);  // WindowRect answered an empty rect: nothing drawn yet
    }

    [Fact]
    public void The_position_option_places_the_screen_s_centre_as_a_fraction_of_the_viewport()
    {
        var s = Screen(new UiScreenOptions { DimBackground = false, Position = (0.25f, 0.5f) });
        s.Open();
        Assert.Equal((480f, 540f, Appearing, 0.5f, 0.5f), FrameScreens().Pos);
    }

    [Fact]
    public void A_position_near_the_edge_keeps_the_whole_screen_inside_the_viewport()
    {
        var s = Screen(new UiScreenOptions { DimBackground = false, Position = (0.99f, 0.01f) });
        s.Open();
        Assert.Equal((1920f - 360, 260f, Appearing, 0.5f, 0.5f), FrameScreens().Pos);
    }

    // ---- MoveTo / Resize ----

    [Fact]
    public void MoveTo_applies_once_on_the_next_frame_with_ImGuiCond_Always()
    {
        var s = Screen();
        s.Open();
        FrameScreens();
        _rect = (600, 280, 720, 520);  // where "ImGui" drew it
        FrameScreens();
        s.MoveTo(0.25f, 0.5f);
        Assert.Equal((480f, 540f, Always, 0.5f, 0.5f), FrameScreens().Pos);
        Assert.Equal(Appearing, FrameScreens().Pos!.Value.Cond);  // the frame after: back to the open's own place
    }

    [Fact]
    public void MoveTo_with_a_pivot_places_that_point_of_the_screen()
    {
        var s = Screen();
        s.Open();
        s.MoveTo(0.5f, 0.5f, pivotX: 0, pivotY: 0);  // the top left corner in the middle
        Assert.Equal((960f, 540f, Always, 0f, 0f), FrameScreens().Pos);
        s.MoveTo(1f, 0.5f, pivotX: 0, pivotY: 0.5f);  // past the right edge: the whole screen stays in
        Assert.Equal((1920f - 720, 540f, Always, 0f, 0.5f), FrameScreens().Pos);
    }

    [Fact]
    public void Resize_applies_once_in_pixels_at_UI_scale_1_and_stays_inside_the_viewport()
    {
        Viewport(2560, 1440, 1.5f);
        var s = Screen();
        s.Open();
        FrameScreens();
        _rect = (100, 100, 1080, 780);
        FrameScreens();
        s.Resize(400, 300);
        var (pos, size) = FrameScreens();
        Assert.Equal((600f, 450f, Always), size);
        Assert.Equal((100f, 100f, Always, 0f, 0f), pos);  // the top left corner stays where it was
        Assert.Equal(Appearing, FrameScreens().Size!.Value.Cond);
        s.Resize(4000, 4000);  // bigger than the display: the display's size
        Assert.Equal((2560f, 1440f, Always), FrameScreens().Size);
    }

    [Fact]
    public void A_resize_that_would_cross_the_edge_moves_the_screen_back_in()
    {
        var s = Screen(new UiScreenOptions { DimBackground = false, Resizable = true });
        s.Open();
        FrameScreens();
        _rect = (1500, 600, 300, 300);
        FrameScreens();
        s.Resize(600, 600);
        var (pos, size) = FrameScreens();
        Assert.Equal((600f, 600f, Always), size);
        Assert.Equal((1920f - 600, 1080f - 600, Always, 0f, 0f), pos);
    }

    [Fact]
    public void Calls_on_a_closed_screen_wait_for_its_next_open()
    {
        var s = Screen();
        s.MoveTo(0.2f, 0.3f);
        s.Resize(500, 400);
        Assert.Equal((null, null), FrameScreens());  // closed: not drawn, the calls wait
        s.Open();
        var (pos, size) = FrameScreens();
        Assert.Equal((384f, 324f, Always, 0.5f, 0.5f), pos);
        Assert.Equal((500f, 400f, Always), size);
        // opened, moved, closed before a frame: the move waits for the next open too
        var (p2, _) = FrameScreens();
        Assert.Equal(Appearing, p2!.Value.Cond);
        s.Close();
        s.MoveTo(0.7f, 0.5f);
        FrameScreens();
        s.Open();
        Assert.Equal((1344f, 540f, Always, 0.5f, 0.5f), FrameScreens().Pos);
    }

    [Fact]
    public void A_later_call_replaces_an_earlier_one_not_yet_applied()
    {
        var s = Screen();
        s.Open();
        s.MoveTo(0.2f, 0.5f);
        s.MoveTo(0.4f, 0.5f);
        Assert.Equal(768f, FrameScreens().Pos!.Value.X);
    }

    [Fact]
    public void Calls_on_a_removed_or_full_screen_screen_do_nothing()
    {
        var gone = Screen();
        gone.Dispose();
        gone.MoveTo(0.1f, 0.1f);
        gone.Resize(100, 100);
        Assert.Null(gone.Placement);
        var full = Screen(new UiScreenOptions { DimBackground = false, FullScreen = true });
        full.Open();
        full.MoveTo(0.1f, 0.1f);
        full.Resize(100, 100);
        var (pos, size) = FrameScreens();
        Assert.Equal((0f, 0f, Always, 0f, 0f), pos);
        Assert.Equal((1920f, 1080f, Always), size);
    }

    [Theory]
    [InlineData(float.NaN, 0.5f, 0.5f, 0.5f)]
    [InlineData(0.5f, float.PositiveInfinity, 0.5f, 0.5f)]
    [InlineData(0.5f, 0.5f, -0.1f, 0.5f)]
    [InlineData(0.5f, 0.5f, 0.5f, 1.5f)]
    public void MoveTo_refuses_values_that_are_no_place(float x, float y, float px, float py)
    {
        var s = Screen();
        Assert.Throws<ArgumentOutOfRangeException>(() => s.MoveTo(x, y, px, py));
        var w = Window();
        Assert.Throws<ArgumentOutOfRangeException>(() => w.MoveTo(x, y, px, py));
    }

    [Theory]
    [InlineData(0f, 100f)]
    [InlineData(100f, -1f)]
    [InlineData(float.NaN, 100f)]
    [InlineData(100f, float.PositiveInfinity)]
    public void Resize_refuses_sizes_that_are_no_size(float w, float h)
    {
        var s = Screen();
        Assert.Throws<ArgumentOutOfRangeException>(() => s.Resize(w, h));
        var win = Window();
        Assert.Throws<ArgumentOutOfRangeException>(() => win.Resize(w, h));
    }

    [Fact]
    public void Placement_reads_where_the_screen_was_drawn_last()
    {
        Viewport(2000, 1000, 2);
        var s = Screen();
        s.Open();
        _rect = (200, 100, 800, 600);
        FrameScreens();
        var p = s.Placement!.Value;
        Assert.Equal(0.3f, p.X, 4);
        Assert.Equal(0.4f, p.Y, 4);
        Assert.Equal(400f, p.Width);
        Assert.Equal(300f, p.Height);
        Assert.Equal(0.1f, p.Left, 4);
        Assert.Equal(0.5f, p.Right, 4);
        Assert.Equal(0.1f, p.Top, 4);
        Assert.Equal(0.7f, p.Bottom, 4);
        // a frame whose window is not visible keeps the last one
        _visible = false;
        _rect = (0, 0, 10, 10);
        FrameScreens();
        Assert.Equal(p, s.Placement);
    }

    // ---- UiWindow ----

    [Fact]
    public void A_window_s_options_still_apply_on_first_use_only()
    {
        var w = Window(new UiWindowOptions { Width = 400, Height = 300, Position = (0.25f, 0.75f) });
        var (pos, size) = FrameWindow(w);
        Assert.Equal((400f, 300f, FirstUseEver), size);
        Assert.Equal((480f, 810f, FirstUseEver, 0.5f, 0.5f), pos);
    }

    [Fact]
    public void A_window_moves_and_resizes_once_on_its_next_frame()
    {
        var w = Window(new UiWindowOptions { Width = 400, Height = 300 });
        FrameWindow(w);
        _rect = (10, 10, 400, 300);
        FrameWindow(w);
        w.MoveTo(0.5f, 0.5f);
        w.Resize(500, 200);
        var (pos, size) = FrameWindow(w);
        Assert.Equal((960f, 540f, Always, 0.5f, 0.5f), pos);
        Assert.Equal((500f, 200f, Always), size);
        Assert.Equal(FirstUseEver, FrameWindow(w).Size!.Value.Cond);
    }

    [Fact]
    public void A_hidden_window_keeps_the_call_for_when_it_shows()
    {
        var w = Window(new UiWindowOptions { StartOpen = false });
        w.MoveTo(0.1f, 0.2f);
        w.Show();
        Assert.Equal((192f, 216f, Always, 0.5f, 0.5f), FrameWindow(w).Pos);
    }

    // ---- remembered placement ----

    [Fact]
    public void A_remembering_screen_keeps_where_the_player_left_it_across_a_restart()
    {
        var o = new UiScreenOptions { DimBackground = false, Resizable = true, RememberPlacement = true };
        var s = Screen(o);
        s.Open();
        FrameScreens();
        _rect = (100, 200, 800, 600);  // the player dragged and resized it
        FrameScreens();
        UiPlacements.Flush();
        var saved = JsonNode.Parse(File.ReadAllText(PlacementFile))!["mods"]!["grimoire"]!["screens"]!["Grimoire"]!;
        Assert.Equal(500.0 / 1920, (double)saved["x"]!, 4);
        Assert.Equal(500.0 / 1080, (double)saved["y"]!, 4);
        Assert.Equal(800.0, (double)saved["width"]!);
        Assert.Equal(600.0, (double)saved["height"]!);
        // the game starts again: a new screen reads the file
        s.Dispose();
        UiPlacements.ResetForTests();
        var again = Screen(o);
        again.Open();
        var (pos, size) = FrameScreens();
        Assert.Equal(500f, pos!.Value.X, 1);
        Assert.Equal(500f, pos.Value.Y, 1);
        Assert.Equal(Appearing, pos.Value.Cond);
        Assert.Equal((800f, 600f, Appearing), size);
    }

    [Fact]
    public void A_remembered_size_applies_only_where_the_player_can_resize()
    {
        Write("""{"mods": {"grimoire": {"screens": {"Grimoire": {"x": 0.25, "y": 0.5, "width": 900, "height": 700}}}}}""");
        var s = Screen(new UiScreenOptions { DimBackground = false, RememberPlacement = true });  // not Resizable
        s.Open();
        var (pos, size) = FrameScreens();
        Assert.Equal((480f, 540f, Appearing, 0.5f, 0.5f), pos);
        Assert.Equal((720f, 520f, Appearing), size);  // the option's size
    }

    [Fact]
    public void Remembered_beats_the_option_and_an_explicit_call_beats_both()
    {
        Write("""{"mods": {"grimoire": {"screens": {"grim": {"x": 0.25, "y": 0.5, "width": 600, "height": 400}}}}}""");
        var s = Screen(new UiScreenOptions
        {
            DimBackground = false, Resizable = true, RememberPlacement = true, PlacementId = "grim", Position = (0.75f, 0.5f),
            Width = 800, Height = 500,
        });
        s.Open();
        var (pos, size) = FrameScreens();
        Assert.Equal((480f, 540f, Appearing, 0.5f, 0.5f), pos);  // the remembered place, not the option's 0.75
        Assert.Equal((600f, 400f, Appearing), size);
        s.MoveTo(0.5f, 0.25f);
        s.Resize(700, 300);
        (pos, size) = FrameScreens();
        Assert.Equal((960f, 270f, Always, 0.5f, 0.5f), pos);
        Assert.Equal((700f, 300f, Always), size);
    }

    [Fact]
    public void A_placement_saved_on_a_bigger_display_is_clamped_into_this_one()
    {
        // saved on 3840x2160 at the right edge, 2400 wide: on 1920x1080 it would hang off the screen
        Write("""{"mods": {"grimoire": {"screens": {"Grimoire": {"x": 0.9, "y": 0.95, "width": 2400, "height": 500}}}}}""");
        var s = Screen(new UiScreenOptions { DimBackground = false, Resizable = true, RememberPlacement = true });
        s.Open();
        var (pos, size) = FrameScreens();
        Assert.Equal((1920f, 500f, Appearing), size);
        Assert.Equal((960f, 1080f - 250, Appearing, 0.5f, 0.5f), pos);
    }

    [Fact]
    public void A_remembering_window_applies_its_place_when_it_appears_and_clamps_it()
    {
        Write("""{"mods": {"grimoire": {"windows": {"Spell list": {"x": 1.4, "y": -0.5, "width": 300, "height": 200}}}}}""");
        var w = Window(new UiWindowOptions { Width = 400, Height = 300, Position = (0.5f, 0.5f), RememberPlacement = true });
        var (pos, size) = FrameWindow(w);
        Assert.Equal((300f, 200f, Appearing), size);
        Assert.Equal((1920f - 150, 100f, Appearing, 0.5f, 0.5f), pos);
        // AutoResize: the contents decide the size, the remembered place still applies
        var auto = Window(new UiWindowOptions { AutoResize = true, RememberPlacement = true });
        (pos, size) = FrameWindow(auto);
        Assert.Null(size);
        Assert.Equal(Appearing, pos!.Value.Cond);
    }

    [Fact]
    public void Screens_and_windows_of_a_mod_are_kept_apart_and_so_are_mods()
    {
        Write("""
        {"mods": {
          "grimoire": {"screens": {"x": {"x": 0.1, "y": 0.5, "width": 100, "height": 100}},
                       "windows": {"x": {"x": 0.9, "y": 0.5, "width": 100, "height": 100}}},
          "betterspells": {"screens": {"x": {"x": 0.5, "y": 0.5, "width": 100, "height": 100}}}}}
        """);
        Assert.Equal(0.1f, UiPlacements.Get("grimoire", window: false, "x")!.Value.X);
        Assert.Equal(0.9f, UiPlacements.Get("grimoire", window: true, "x")!.Value.X);
        Assert.Equal(0.5f, UiPlacements.Get("betterspells", window: false, "x")!.Value.X);
        Assert.Null(UiPlacements.Get("betterspells", window: true, "x"));
    }

    [Fact]
    public void A_missing_file_remembers_nothing_and_is_written_on_the_first_change()
    {
        Assert.False(File.Exists(PlacementFile));
        Assert.Null(UiPlacements.Get("grimoire", window: false, "Grimoire"));
        UiPlacements.Put("grimoire", window: false, "Grimoire", new UiPlacements.Entry(0.5f, 0.5f, 720, 520));
        UiPlacements.Flush();
        Assert.True(File.Exists(PlacementFile));
        Assert.False(File.Exists(PlacementFile + ".tmp"));
    }

    [Fact]
    public void A_corrupt_file_is_kept_aside_with_a_warning_and_nothing_is_remembered()
    {
        Write("""{"mods": {"grimoire": {"screens": {"Grimoire": {"x": 0.25""");  // cut off
        Assert.Null(UiPlacements.Get("grimoire", window: false, "Grimoire"));
        Assert.Contains(_backend.Logs, l => l.StartsWith("Warn", StringComparison.Ordinal) && l.Contains("ui-placement.json", StringComparison.Ordinal));
        Assert.True(File.Exists(PlacementFile + ".bad"));
        // the next change writes a good file again
        UiPlacements.Put("grimoire", window: false, "Grimoire", new UiPlacements.Entry(0.3f, 0.4f, 500, 400));
        UiPlacements.Flush();
        UiPlacements.ResetForTests();
        Assert.Equal(new UiPlacements.Entry(0.3f, 0.4f, 500, 400), UiPlacements.Get("grimoire", window: false, "Grimoire"));
    }

    [Theory]
    [InlineData("""[1, 2, 3]""")]
    [InlineData("""{"mods": 5}""")]
    [InlineData("""{"mods": {"grimoire": {"screens": {"Grimoire": {"x": "left", "y": 0.5, "width": 100, "height": 100}}}}}""")]
    [InlineData("""{"mods": {"grimoire": {"screens": {"Grimoire": {"x": 0.5, "y": 0.5, "width": -100, "height": 100}}}}}""")]
    [InlineData("""{"mods": {"grimoire": {"screens": {"Grimoire": {"x": 0.5, "y": 0.5, "width": 100}}}}}""")]
    [InlineData("""{"mods": {"grimoire": {"screens": {"Grimoire": {"x": 1e39, "y": 0.5, "width": 100, "height": 100}}}}}""")]
    [InlineData("")]
    public void Entries_that_are_no_placement_are_skipped(string json)
    {
        Write(json);
        Assert.Null(UiPlacements.Get("grimoire", window: false, "Grimoire"));
        var s = Screen(new UiScreenOptions { DimBackground = false, RememberPlacement = true });
        s.Open();
        Assert.Equal((960f, 540f, Appearing, 0.5f, 0.5f), FrameScreens().Pos);  // the options' place
    }

    [Fact]
    public void A_hand_edited_file_with_a_key_twice_neither_throws_nor_loses_the_other_entries()
    {
        // a player pasted an entry in again: JSON with a duplicate property name
        Write("""
        {"mods": {"grimoire": {"screens": {
          "Grimoire": {"x": 0.2, "y": 0.5, "width": 600, "height": 400},
          "Grimoire": {"x": 0.25, "y": 0.5, "width": 600, "height": 400},
          "Other": {"x": 0.7, "y": 0.5, "width": 300, "height": 200}}}}}
        """);
        var s = Screen(new UiScreenOptions { DimBackground = false, Resizable = true, RememberPlacement = true });  // reads the file
        s.Open();
        Assert.Equal((480f, 540f, Appearing, 0.5f, 0.5f), FrameScreens().Pos);  // the last one wins, as a JSON reader reads it
        Assert.Equal(new UiPlacements.Entry(0.7f, 0.5f, 300, 200), UiPlacements.Get("grimoire", window: false, "Other"));
    }

    [Fact]
    public void A_file_that_cannot_be_read_is_not_overwritten_this_start()
    {
        if (OperatingSystem.IsWindows()) return;  // the unreadable file is made with Unix permissions
        const string json = """{"mods": {"betterspells": {"screens": {"Better Spells": {"x": 0.3, "y": 0.5, "width": 600, "height": 400}}}}}""";
        Write(json);
        File.SetUnixFileMode(PlacementFile, UnixFileMode.None);  // as a file another program holds locked
        try
        {
            if (TryRead(PlacementFile)) return;  // a user who may read anything (root): no unreadable file to test with
            var s = Screen(new UiScreenOptions { DimBackground = false, RememberPlacement = true });
            s.Open();
            FrameScreens();
            _rect = (100, 200, 720, 520);
            FrameScreens();
            UiPlacements.Flush();
            Assert.Contains(_backend.Logs, l => l.StartsWith("Warn", StringComparison.Ordinal) && l.Contains("ui-placement.json", StringComparison.Ordinal));
        }
        finally
        {
            File.SetUnixFileMode(PlacementFile, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        }
        Assert.Equal(json, File.ReadAllText(PlacementFile));  // Better Spells' placement is still there
    }

    private static bool TryRead(string path)
    {
        try
        {
            File.ReadAllText(path);
            return true;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    [Fact]
    public void A_write_that_fails_is_tried_again_at_the_next_flush()
    {
        // the write fails once (here a folder where the temp file goes; in a game a virus scanner or a sync tool holding the
        // file for a moment): the placement stays to be written, so the next flush (the next change, a mod's unload, the
        // game's quit) still saves it
        Directory.CreateDirectory(PlacementFile + ".tmp");
        UiPlacements.Put("grimoire", window: false, "Grimoire", new UiPlacements.Entry(0.3f, 0.4f, 500, 400));
        UiPlacements.Flush();
        Assert.False(File.Exists(PlacementFile));
        Assert.Contains(_backend.Logs, l => l.StartsWith("Warn", StringComparison.Ordinal) && l.Contains("ui-placement.json", StringComparison.Ordinal));
        Directory.Delete(PlacementFile + ".tmp");
        UiPlacements.Flush();
        Assert.True(File.Exists(PlacementFile));
        UiPlacements.ResetForTests();
        Assert.Equal(new UiPlacements.Entry(0.3f, 0.4f, 500, 400), UiPlacements.Get("grimoire", window: false, "Grimoire"));
    }

    [Fact]
    public void A_file_a_newer_Kiln_wrote_is_read_and_left_as_it_is()
    {
        // the player went back from a newer Kiln (the beta channel) whose file has a version this one does not know: what
        // this one can read is used, and the file, with what only the newer Kiln reads, is not written over this start
        const string json = """{"version": 2, "mods": {"grimoire": {"screens": {"Grimoire": {"x": 0.25, "y": 0.5, "width": 600, "height": 400, "monitor": 1}}}}, "layouts": {}}""";
        Write(json);
        var s = Screen(new UiScreenOptions { DimBackground = false, Resizable = true, RememberPlacement = true });
        s.Open();
        Assert.Equal((480f, 540f, Appearing, 0.5f, 0.5f), FrameScreens().Pos);
        _rect = (100, 200, 800, 600);
        FrameScreens();
        UiPlacements.Flush();
        Assert.Equal(json, File.ReadAllText(PlacementFile));
        Assert.Contains(_backend.Logs, l => l.StartsWith("Warn", StringComparison.Ordinal) && l.Contains("newer Kiln", StringComparison.Ordinal));
        // a file of this version (or one without a version, written by hand) is written as before
        UiPlacements.ResetForTests();
        Write("""{"version": 1, "mods": {}}""");
        UiPlacements.Put("grimoire", window: false, "Grimoire", new UiPlacements.Entry(0.3f, 0.4f, 500, 400));
        UiPlacements.Flush();
        Assert.Contains("\"grimoire\"", File.ReadAllText(PlacementFile), StringComparison.Ordinal);
    }

    [Fact]
    public void A_mod_that_unloads_writes_its_placement_at_once()
    {
        // the player drags the screen and the game quits (the host unloads every mod) before the debounce runs out
        var s = Screen(new UiScreenOptions { DimBackground = false, Resizable = true, RememberPlacement = true });
        s.Open();
        FrameScreens();
        _rect = (100, 200, 800, 600);
        FrameScreens();
        Assert.False(File.Exists(PlacementFile));  // Debounce is an hour here
        UiEngine.Retire(_mod);
        Assert.True(File.Exists(PlacementFile));
        var saved = JsonNode.Parse(File.ReadAllText(PlacementFile))!["mods"]!["grimoire"]!["screens"]!["Grimoire"]!;
        Assert.Equal(800.0, (double)saved["width"]!);
    }

    [Fact]
    public void A_good_entry_beside_a_bad_one_is_still_read()
    {
        Write("""{"mods": {"grimoire": {"screens": {"bad": {"x": null}, "good": {"x": 0.2, "y": 0.3, "width": 100, "height": 50}}}}}""");
        Assert.Equal(new UiPlacements.Entry(0.2f, 0.3f, 100, 50), UiPlacements.Get("grimoire", window: false, "good"));
    }

    [Fact]
    public void A_screen_that_does_not_remember_writes_nothing()
    {
        var s = Screen();
        s.Open();
        _rect = (100, 200, 800, 600);
        FrameScreens();
        UiPlacements.Flush();
        Assert.False(File.Exists(PlacementFile));
    }

    [Fact]
    public void Remembering_needs_an_id()
    {
        Assert.Throws<ArgumentException>(() => new UiScreen("", _ => { }, new UiScreenOptions { RememberPlacement = true }, _mod));
        Assert.Throws<ArgumentException>(() => new UiScreen("Title", _ => { }, new UiScreenOptions { RememberPlacement = true, PlacementId = " " }, _mod));
        using var ok = new UiScreen("", _ => { }, new UiScreenOptions { RememberPlacement = true, PlacementId = "main" }, _mod);
    }

    [Fact]
    public void The_default_id_is_the_title_in_English()
    {
        var o = new UiScreenOptions { DimBackground = false, RememberPlacement = true };
        var s = UiEngine.AddScreen(new UiScreen(UiText.Of(("en", "Spells"), ("de", "Zauber")), _ => { }, o, _mod));
        _made.Add(s);
        s.Open();
        _rect = (100, 100, 720, 520);
        FrameScreens();
        Assert.NotNull(UiPlacements.Get("grimoire", window: false, "Spells"));
    }

    [Fact]
    public void Small_jitter_is_not_a_change()
    {
        var s = Screen(new UiScreenOptions { DimBackground = false, RememberPlacement = true });
        s.Open();
        _rect = (100, 100, 720, 520);
        FrameScreens();
        UiPlacements.Flush();
        var written = File.GetLastWriteTimeUtc(PlacementFile);
        File.Delete(PlacementFile);
        _rect = (100.1f, 100.1f, 720, 520);
        FrameScreens();
        UiPlacements.Flush();
        Assert.False(File.Exists(PlacementFile));
        _ = written;
    }

    private void Write(string json) => File.WriteAllText(PlacementFile, json);
}
