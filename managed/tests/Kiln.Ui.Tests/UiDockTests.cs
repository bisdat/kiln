// SPDX-License-Identifier: MIT
// Window docking across mods (GitHub issue 149): Ui.FindWindow and its handles, UiWindowOptions.AcceptDock, DockTo /
// Undock / DockedTo / DockChanged, and Kiln.Ui's frame doing the rest: the target drawn first and the docked window placed
// from its rect of the same frame, the window the player drags or resizes leading its group, Shift to drag one alone, a
// drop near the old side docking again, the group kept inside the viewport, a docked window waiting for its target when
// both open together, targets that close, unload and come back, loops, and what RememberPlacement keeps. Also
// UiDraw.WindowMovedByUser / WindowResizedByUser and HeldMods.
//
// No game: a fake kiln.ui table answers Begin / SetNextWindowPos / SetNextWindowSize / WindowRect / window_state from a
// small model of Dear ImGui's windows (conditions Always, FirstUseEver, Appearing and the pivot; a player's drag applied
// before the window's Begin as ImGui's NewFrame does, a player's resize inside it). The real ImGui side of this is
// native/services/ui/tests/window_state_test.cpp.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using Kiln.FakeUi;
using Xunit;

using Kiln;

namespace UiDockTestSuite;

[Collection(UiSerial.Name)]
public sealed unsafe class UiDockTests : IDisposable
{
    private const uint Always = 1, FirstUseEver = 4, Appearing = 8;

    // ---- the model of ImGui's windows ----
    private sealed class Win
    {
        public float X = 60, Y = 60, W, H;
        public long LastFrame = -10;
        public bool Ever, Moving, Resizing;
        public (float X, float Y)? DragTo;           // the player's drag, applied before Begin ("NewFrame")
        public (float X, float Y, float W, float H)? ResizeTo;  // the player's resize, applied inside Begin
        // the player holds the right border at this x: inside Begin, after any SetNextWindowPos, ImGui keeps that edge at
        // the pointer and the window's left edge where it is (UpdateWindowManualResize), every frame while held
        public float? RightEdgeAt;
    }

    private static readonly Dictionary<string, Win> Wins = [];
    private static readonly List<string> Begun = [];
    private static (float X, float Y, uint Cond, float Px, float Py)? _nextPos;
    private static (float W, float H, uint Cond)? _nextSize;
    private static string? _current;
    private static long _frame;
    private static uint _mods;
    private static uint _currentFlags;
    private static long _now = 1_000_000;

    private readonly IKilnBackend _previous = Backend.Current;
    private readonly string _previousRoot = KilnPaths.Root;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-dock-" + Guid.NewGuid().ToString("N"));
    private readonly List<IDisposable> _made = [];
    private readonly FakeUiTable.Backend _backend = new();
    private readonly ModContext _spells, _grimoire;

    public UiDockTests()
    {
        FakeUiTable.Install();
        ref var t = ref FakeUiTable.Table->Base.Base.Base;
        t.SetNextWindowPos = &SetPos;
        t.SetNextWindowSize = &SetSize;
        t.Begin = &Begin;
        t.WindowRect = &GetRect;
        FakeUiTable.Table->Base.KeyMods = &Mods;
        Backend.Current = _backend;
        UiNative.ResetForTests();
        UiNative.WindowStateOverride = &State;
        Directory.CreateDirectory(Path.Combine(_root, "config"));
        KilnPaths.Root = _root;
        KilnPaths.Config = null!;
        UiPlacements.ResetForTests();
        UiPlacements.Debounce = TimeSpan.FromHours(1);
        UiDocking.ResetForTests();
        UiDocking.Clock = () => _now;
        _spells = HostAccess.CreateContext(new ModInfo("betterspells", "Better Spells", "1.1.5", _root), 0, _root);
        _grimoire = HostAccess.CreateContext(new ModInfo("grimoire", "Grimoire", "1.0.4", _root), 0, _root);
        UiEngine.DrawContext.Frame = new UiNative.Frame { Width = 1920, Height = 1080, DpiScale = 1 };
        Wins.Clear();
        Begun.Clear();
        _nextPos = null;
        _nextSize = null;
        _current = null;
        _frame = 0;
        _mods = 0;
        _currentFlags = 0;
    }

    public void Dispose()
    {
        foreach (var d in _made) d.Dispose();
        UiEngine.Retire(_spells);
        UiEngine.Retire(_grimoire);
        UiDocking.ResetForTests();
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
    private static void SetPos(float x, float y, uint cond, float px, float py) => _nextPos = (x, y, cond, px, py);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void SetSize(float w, float h, uint cond) => _nextSize = (w, h, cond);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint Mods() => _mods;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte Begin(NativeStr title, byte* open, uint flags)
    {
        var key = title.ToString();
        Begun.Add(key);
        if (!Wins.TryGetValue(key, out var w)) Wins[key] = w = new Win();
        var appearing = w.LastFrame != _frame - 1;
        bool Applies(uint cond) => cond == Always || (cond == Appearing && appearing) || (cond == FirstUseEver && !w.Ever);
        if (_nextSize is { } z && Applies(z.Cond)) (w.W, w.H) = (z.W, z.H);
        if (w.W <= 0) w.W = 320;
        if (w.H <= 0) w.H = 240;
        if (_nextPos is { } p && Applies(p.Cond)) (w.X, w.Y) = (p.X - p.Px * w.W, p.Y - p.Py * w.H);
        if (w.ResizeTo is { } r) (w.X, w.Y, w.W, w.H) = r;
        w.ResizeTo = null;
        if (w.Resizing && w.RightEdgeAt is { } edge) w.W = edge - w.X;
        w.Ever = true;
        w.LastFrame = _frame;
        _nextPos = null;
        _nextSize = null;
        _current = key;
        return 1;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void GetRect(float* x, float* y, float* w, float* h)
    {
        var r = _current is { } k ? Wins[k] : new Win { X = 0, Y = 0 };
        (*x, *y, *w, *h) = (r.X, r.Y, r.W, r.H);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte State(NativeStr name, UiNative.WindowState* s)
    {
        var key = name.ToString();
        if (key.Length == 0)
        {
            s->Flags = _currentFlags;
            return _currentFlags != 0 ? (byte)1 : (byte)0;
        }
        if (!Wins.TryGetValue(key, out var w) || !w.Ever) return 0;
        s->Flags = UiNative.WindowExists | (w.Moving ? UiNative.WindowMoving : 0) | (w.Resizing ? UiNative.WindowResizing : 0);
        (s->X, s->Y, s->Width, s->Height) = (w.X, w.Y, w.W, w.H);
        return 1;
    }

    // ---- helpers ----

    private UiWindow Window(ModContext mod, string title, UiWindowOptions? o = null, Action<UiDraw>? draw = null)
    {
        var w = UiEngine.AddWindow(new UiWindow(title, draw ?? (_ => { }), o ?? new UiWindowOptions(), mod));
        _made.Add(w);
        return w;
    }

    private UiWindow Spells(bool accept = true, string id = "spells", bool remember = false) =>
        Window(_spells, "Better Spells", new UiWindowOptions { Width = 300, Height = 200, Position = (0.25f, 0.5f), AcceptDock = accept, PlacementId = id, RememberPlacement = remember });

    private UiWindow Grimoire(bool remember = false) =>
        Window(_grimoire, "Grimoire", new UiWindowOptions { Width = 240, Height = 160, Position = (0.75f, 0.25f), PlacementId = "grimoire", RememberPlacement = remember });

    private static UiWindowHandle Handle(string mod = "betterspells", string id = "spells") => Ui.FindWindow(mod, id)!;

    /// <summary>One overlay frame of the windows: the player's drags land first (ImGui's NewFrame), then Kiln.Ui draws.</summary>
    private static void Frame()
    {
        _frame++;
        _now += 16;
        foreach (var w in Wins.Values)
            if (w.DragTo is { } d)
            {
                (w.X, w.Y) = d;
                w.DragTo = null;
            }
        Begun.Clear();
        UiEngine.DrawWindows(UiEngine.DrawContext);
    }

    private static Win Of(UiWindow w) => Wins[w.Key];

    /// <summary>The player holds the window and drags its top left corner to (x, y) pixels, from the next frame.</summary>
    private static void Drag(UiWindow w, float x, float y)
    {
        var m = Of(w);
        m.Moving = true;
        m.DragTo = (x, y);
    }

    private static void Release(UiWindow w) => Of(w).Moving = false;

    private static void At(Win w, float x, float y)
    {
        Assert.Equal(x, w.X, 3);
        Assert.Equal(y, w.Y, 3);
    }

    private static void AssertRightOf(UiWindow target, UiWindow docked, float offset = 0)
    {
        Win t = Of(target), d = Of(docked);
        Assert.Equal(t.X + t.W, d.X, 3);
        Assert.Equal(t.Y + offset, d.Y, 3);
    }

    // ---- handles and opt-in ----

    [Fact]
    public void AcceptDock_is_off_by_default()
    {
        Assert.False(new UiWindowOptions().AcceptDock);
    }

    [Fact]
    public void FindWindow_finds_only_a_window_that_accepts_docks()
    {
        var spells = Spells();
        Window(_spells, "Spell book", new UiWindowOptions { PlacementId = "book" });
        var h = Ui.FindWindow("betterspells", "spells");
        Assert.NotNull(h);
        Assert.Equal("betterspells", h!.ModId);
        Assert.Equal("spells", h.PlacementId);
        Assert.True(h.Available);
        Assert.True(h.Open);
        Assert.Null(h.Placement);  // not drawn yet
        Assert.Equal(h, Ui.FindWindow("betterspells", "spells"));
        Assert.Null(Ui.FindWindow("betterspells", "book"));  // no AcceptDock: not found, not dockable, not readable
        Assert.Null(Ui.FindWindow("betterspells", "nothing"));
        Assert.Null(Ui.FindWindow("someone", "spells"));
        Frame();
        Assert.Equal(spells.Placement, h.Placement);
        spells.Hide();
        Assert.False(h.Open);
        Assert.True(h.Available);
    }

    [Fact]
    public void A_window_without_a_placement_id_is_found_by_its_title()
    {
        Window(_spells, "Better Spells", new UiWindowOptions { AcceptDock = true });
        Assert.NotNull(Ui.FindWindow("betterspells", "Better Spells"));
    }

    [Fact]
    public void DockTo_refuses_the_window_itself_loops_and_bad_values()
    {
        var spells = Spells();
        var book = Window(_grimoire, "Grimoire", new UiWindowOptions { AcceptDock = true, PlacementId = "grimoire" });
        var grimoire = Ui.FindWindow("grimoire", "grimoire")!;
        Assert.Throws<ArgumentNullException>(() => book.DockTo(null!, DockSide.Right));
        Assert.Throws<ArgumentException>(() => book.DockTo(grimoire, DockSide.Right));
        Assert.Throws<ArgumentOutOfRangeException>(() => book.DockTo(Handle(), (DockSide)7));
        Assert.Throws<ArgumentOutOfRangeException>(() => book.DockTo(Handle(), DockSide.Right, float.NaN));
        book.DockTo(Handle(), DockSide.Right);
        Assert.Throws<InvalidOperationException>(() => spells.DockTo(grimoire, DockSide.Left));  // spells <- book <- spells
        Assert.Null(spells.DockedTo);
        Assert.Equal(Handle(), book.DockedTo);
    }

    [Fact]
    public void DockTo_and_Undock_raise_DockChanged_as_the_mod_s_own()
    {
        Spells();
        var book = Grimoire();
        var seen = new List<UiDockChange>();
        book.DockChanged += c => seen.Add(c);
        book.DockTo(Handle(), DockSide.Right, 12);
        book.DockTo(Handle(), DockSide.Right, 12);  // the same dock again: no change
        book.Undock();
        book.Undock();
        Assert.Equal(2, seen.Count);
        Assert.Equal(Handle(), seen[0].DockedTo);
        Assert.Equal(DockSide.Right, seen[0].Side);
        Assert.Equal(12, seen[0].Offset);
        Assert.False(seen[0].ByPlayer);
        Assert.Null(seen[1].DockedTo);
        Assert.Null(book.DockedTo);
    }

    // ---- the same frame ----

    [Fact]
    public void Windows_without_docks_are_drawn_as_before_in_their_order()
    {
        var a = Spells();
        var b = Grimoire();
        Frame();
        Assert.Equal(new[] { a.Key, b.Key }, Begun);
        Assert.Equal(0.25f * 1920 - 150, Of(a).X, 3);
        Assert.Equal(0.75f * 1920 - 120, Of(b).X, 3);
    }

    [Fact]
    public void A_docked_window_appears_beside_its_target_in_their_first_frame_whatever_their_order()
    {
        var book = Grimoire();  // made first: drawn first without docking
        var spells = Spells();
        book.DockTo(Handle(), DockSide.Right, 10);
        Frame();
        Assert.Equal(new[] { spells.Key, book.Key }, Begun);  // the target first
        AssertRightOf(spells, book, 10);
        Assert.Equal(spells.Placement!.Value.Right, book.Placement!.Value.Left, 4);
    }

    [Fact]
    public void Every_side_and_its_offset()
    {
        var spells = Spells();
        var book = Grimoire();
        Frame();
        Win t = Of(spells), d = Of(book);
        book.DockTo(Handle(), DockSide.Left, 5);
        Frame();
        At(d, t.X - d.W, t.Y + 5);
        book.DockTo(Handle(), DockSide.Top, 7);
        Frame();
        At(d, t.X + 7, t.Y - d.H);
        book.DockTo(Handle(), DockSide.Bottom, -3);
        Frame();
        At(d, t.X - 3, t.Y + t.H);
        UiEngine.DrawContext.Frame = new UiNative.Frame { Width = 1920, Height = 1080, DpiScale = 2 };  // offsets are at UI scale 1
        book.DockTo(Handle(), DockSide.Right, 10);
        Frame();
        At(d, t.X + t.W, t.Y + 20);
    }

    [Fact]
    public void Moving_or_resizing_the_target_moves_the_docked_window_in_the_same_frame()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        spells.MoveTo(0.5f, 0.5f, 0, 0);
        Frame();
        Assert.Equal(960, Of(spells).X, 3);
        AssertRightOf(spells, book);
        Drag(spells, 400, 300);
        Frame();
        Assert.Equal((400f, 300f), (Of(spells).X, Of(spells).Y));
        AssertRightOf(spells, book);
        Release(spells);
        Of(spells).Resizing = true;
        Of(spells).ResizeTo = (400, 300, 420, 200);  // its right edge out by 120
        Frame();
        AssertRightOf(spells, book);
        Assert.Equal(820, Of(book).X, 3);
    }

    [Fact]
    public void Dragging_the_docked_window_moves_its_target_with_it_in_the_same_frame()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right, 10);
        Frame();
        var before = Begun.ToArray();
        Drag(book, 1000, 500);
        Frame();
        Assert.Equal(new[] { book.Key, spells.Key }, Begun);  // the dragged one leads
        Assert.Equal((1000f, 500f), (Of(book).X, Of(book).Y));
        AssertRightOf(spells, book, 10);
        Release(book);
        Frame();
        Assert.Equal(before, Begun);
        AssertRightOf(spells, book, 10);
        Assert.Equal(Handle(), book.DockedTo);
    }

    [Fact]
    public void A_docked_window_the_player_resizes_at_the_shared_edge_leads_so_the_edge_follows_the_pointer()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        var d = Of(book);
        d.Resizing = true;
        d.ResizeTo = (d.X - 50, d.Y, d.W + 50, d.H);  // its left edge, the shared one, 50 to the left
        var right = d.X + d.W;
        Frame();
        Assert.Equal(book.Key, Begun[0]);
        Assert.Equal(right, d.X + d.W, 3);
        AssertRightOf(spells, book);  // the target moved along
        d.Resizing = false;
        Frame();
        AssertRightOf(spells, book);
        Assert.Equal(right, d.X + d.W, 3);
    }

    [Fact]
    public void MoveTo_on_the_docked_window_moves_the_pair()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        book.MoveTo(0.5f, 0.5f, 0, 0);
        Frame();
        Assert.Equal((960f, 540f), (Of(book).X, Of(book).Y));
        AssertRightOf(spells, book);
    }

    // ---- Shift and snapping ----

    [Fact]
    public void Shift_drag_moves_the_docked_window_alone_and_undocks_it()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        var seen = new List<UiDockChange>();
        book.DockChanged += c => seen.Add(c);
        var (tx, ty) = (Of(spells).X, Of(spells).Y);
        _mods = (uint)KeyMods.Shift;
        Drag(book, 1200, 700);
        Frame();
        Assert.Null(book.DockedTo);
        Assert.Equal((1200f, 700f), (Of(book).X, Of(book).Y));
        Assert.Equal((tx, ty), (Of(spells).X, Of(spells).Y));
        Assert.Single(seen);
        Assert.True(seen[0].ByPlayer);
        Assert.Null(seen[0].DockedTo);
        _mods = 0;
        Drag(spells, 100, 100);  // apart now: the target moves alone too
        Frame();
        Assert.Equal((1200f, 700f), (Of(book).X, Of(book).Y));
    }

    [Fact]
    public void Shift_drag_of_the_target_leaves_the_docked_window_where_it_is()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        var (bx, by) = (Of(book).X, Of(book).Y);
        _mods = (uint)KeyMods.Shift;
        Drag(spells, 50, 50);
        Frame();
        Assert.Null(book.DockedTo);
        Assert.Equal((bx, by), (Of(book).X, Of(book).Y));
    }

    [Fact]
    public void A_window_the_player_undocked_docks_again_when_dropped_near_its_old_side()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        _mods = (uint)KeyMods.Shift;
        Drag(book, 1200, 700);
        Frame();
        _mods = 0;
        var seen = new List<UiDockChange>();
        book.DockChanged += c => seen.Add(c);
        var t = Of(spells);
        Drag(book, t.X + t.W + 30, t.Y + 40);  // 30 px off: too far
        Frame();
        Release(book);
        Frame();
        Assert.Null(book.DockedTo);
        Drag(book, t.X + t.W + 12, t.Y + 40);  // 12 px off, overlapping along the edge
        Frame();
        Release(book);
        Frame();  // the drop
        Assert.Equal(Handle(), book.DockedTo);
        Assert.Single(seen);
        Assert.True(seen[0].ByPlayer);
        Assert.Equal(DockSide.Right, seen[0].Side);
        Assert.Equal(40, seen[0].Offset, 3);
        Frame();
        AssertRightOf(spells, book, 40);
    }

    [Fact]
    public void A_window_shown_on_the_other_side_docks_again_when_dropped_there()
    {
        var spells = Window(_spells, "Better Spells", new UiWindowOptions { Width = 300, Height = 200, Position = (0.95f, 0.5f), AcceptDock = true, PlacementId = "spells" });
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        var t = Of(spells);
        Assert.Equal(t.X - Of(book).W, Of(book).X, 3);  // shown on its left: no room on the right
        _mods = (uint)KeyMods.Shift;
        Drag(book, 300, 100);
        Frame();
        _mods = 0;
        Release(book);
        Frame();
        Assert.Null(book.DockedTo);
        var seen = new List<UiDockChange>();
        book.DockChanged += c => seen.Add(c);
        Drag(book, t.X - Of(book).W - 6, t.Y + 20);  // back where it was shown, 6 px off
        Frame();
        Release(book);
        Frame();  // the drop
        Assert.Equal(Handle(), book.DockedTo);
        Assert.Equal(DockSide.Left, Assert.Single(seen).Side);
        Frame();
        Assert.Equal(t.X - Of(book).W, Of(book).X, 3);
        Assert.Equal(t.Y + 20, Of(book).Y, 3);
    }

    [Fact]
    public void A_drop_never_docks_again_over_an_Undock_from_another_thread()
    {
        Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        var link = book.Dock.Link;
        Assert.True(book.Dock.PlayerSet(link, link, null, link));  // the player's Shift drag: undocked, its snap kept
        var read = book.Dock.Snap;  // the render thread reads it at the drop
        book.Undock();               // the mod on another thread, before the drop docks it again
        Assert.False(book.Dock.PlayerSet(null, read, read, read));
        Assert.Null(book.DockedTo);
        Assert.Null(book.Dock.Snap);
    }

    [Fact]
    public void After_Undock_from_the_mod_a_drop_near_the_old_side_does_not_dock()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        book.Undock();
        var t = Of(spells);
        Drag(book, t.X + t.W + 2, t.Y);
        Frame();
        Release(book);
        Frame();
        Assert.Null(book.DockedTo);
    }

    // ---- the viewport ----

    [Fact]
    public void A_docked_window_that_does_not_fit_on_its_side_goes_on_the_other_side()
    {
        var spells = Window(_spells, "Better Spells", new UiWindowOptions { Width = 300, Height = 200, Position = (0.95f, 0.5f), AcceptDock = true, PlacementId = "spells" });
        Frame();  // the target alone, at the right edge
        var book = Grimoire();
        Frame();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        Win t = Of(spells), d = Of(book);
        Assert.Equal(t.X - d.W, d.X, 3);  // left of it: the right side is off the screen
        Assert.Equal(t.Y, d.Y, 3);
        Assert.True(t.X + t.W <= 1920);
        Assert.Equal(Handle(), book.DockedTo);  // still the dock the mod asked for
    }

    [Fact]
    public void Two_new_windows_with_the_target_near_the_edge_open_with_the_docked_one_on_the_side_that_fits()
    {
        var spells = Window(_spells, "Better Spells", new UiWindowOptions { Width = 300, Height = 200, Position = (0.9f, 0.5f), AcceptDock = true, PlacementId = "spells" });
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();  // the first frame of both
        Win t = Of(spells), d = Of(book);
        Assert.Equal(t.X - d.W, d.X, 3);
        Assert.Equal(0.9f * 1920 - 150, t.X, 3);  // the target where its options put it
        Drag(spells, 200, 300);  // the side stays while the dock holds
        Frame();
        Assert.Equal(t.X - d.W, d.X, 3);
    }

    [Fact]
    public void A_pair_dragged_against_the_edge_stays_inside_the_viewport()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        Drag(spells, 1800, 100);  // the docked window would end past 1920
        Frame();
        Win t = Of(spells), d = Of(book);
        Assert.Equal(1920, d.X + d.W, 3);
        AssertRightOf(spells, book);
        Release(spells);
        Drag(book, -400, 100);  // its target would start left of 0
        Frame();
        Assert.Equal(0, t.X, 3);
        AssertRightOf(spells, book);
    }

    [Fact]
    public void A_resize_that_pushes_the_docked_window_out_does_not_drag_the_resized_window_along()
    {
        var spells = Spells();  // 330..630
        var book = Grimoire();  // 240 wide, docked on its right
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        var t = Of(spells);
        t.Resizing = true;
        t.RightEdgeAt = 1800;  // the player holds the right border there: Grimoire would end at 2040
        for (var i = 0; i < 6; i++)
        {
            Frame();
            Assert.Equal(330, t.X, 3);  // its left edge stays where it was, the right one at the pointer
            Assert.Equal(1800, t.X + t.W, 3);
            AssertRightOf(spells, book);
        }
        t.Resizing = false;
        t.RightEdgeAt = null;
        Frame();  // let go: the pair goes back inside
        Assert.Equal(1920, Of(book).X + Of(book).W, 3);
        AssertRightOf(spells, book);
        Assert.Equal(1470, t.W, 3);
    }

    [Fact]
    public void A_docked_window_can_never_push_the_window_it_docks_to_off_the_screen()
    {
        var spells = Spells();  // 330, 440, 300 x 200
        Frame();
        var hostile = Window(_grimoire, "Far", new UiWindowOptions { Width = 100, Height = 100 });
        hostile.DockTo(Handle(), DockSide.Left, -5000);  // far above it
        var wall = Window(_grimoire, "Wall", new UiWindowOptions { Width = 1900, Height = 100 });
        wall.DockTo(Handle(), DockSide.Left);  // wider than the room beside it on either side
        for (var i = 0; i < 3; i++) Frame();
        var t = Of(spells);
        Assert.Equal((330f, 440f), (t.X, t.Y));
        Assert.Equal(t.X, Of(wall).X + Of(wall).W, 3);  // still docked, as far in as its target allows
        Assert.Equal(t.Y - 5000, Of(hostile).Y, 3);
    }

    [Fact]
    public void A_group_too_big_for_the_viewport_keeps_the_window_the_others_dock_to_inside()
    {
        var spells = Window(_spells, "Better Spells", new UiWindowOptions { Width = 1000, Height = 200, Position = (0.3f, 0.5f), AcceptDock = true, PlacementId = "spells" });
        var book = Window(_grimoire, "Grimoire", new UiWindowOptions { Width = 1000, Height = 160 });
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        Frame();
        var t = Of(spells);
        Assert.Equal(0.3f * 1920 - 500, t.X, 3);  // where its options put it: Grimoire hangs over the right edge
        AssertRightOf(spells, book);
        Drag(spells, 600, 300);  // the player still drags it anywhere inside
        Frame();
        Assert.Equal((600f, 300f), (t.X, t.Y));
        AssertRightOf(spells, book);
        Drag(spells, 1000, 300);  // but never out
        Frame();
        Assert.Equal(920, t.X, 3);
        AssertRightOf(spells, book);
    }

    // ---- opening, closing, unloading ----

    [Fact]
    public void A_docked_window_opened_before_its_target_waits_for_it_and_appears_beside_it()
    {
        var spells = Window(_spells, "Better Spells", new UiWindowOptions { Width = 300, Height = 200, Position = (0.25f, 0.5f), AcceptDock = true, PlacementId = "spells", StartOpen = false });
        var book = Window(_grimoire, "Grimoire", new UiWindowOptions { Width = 240, Height = 160, StartOpen = false });
        book.DockTo(Handle(), DockSide.Right);
        book.Show();  // the same key press: Grimoire's handler ran first
        Frame();
        Assert.Empty(Begun);  // not at its own place first
        Assert.Null(book.Placement);
        spells.Show();
        Frame();
        Assert.Equal(new[] { spells.Key, book.Key }, Begun);
        AssertRightOf(spells, book);
    }

    [Fact]
    public void A_docked_window_waits_at_most_a_quarter_second_for_a_closed_target()
    {
        Window(_spells, "Better Spells", new UiWindowOptions { AcceptDock = true, PlacementId = "spells", StartOpen = false });
        var book = Window(_grimoire, "Grimoire", new UiWindowOptions { Width = 240, Height = 160, StartOpen = false });
        book.DockTo(Handle(), DockSide.Right);
        book.Show();
        var waited = 0;
        for (var i = 0; i < 40 && !Begun.Contains(book.Key); i++)
        {
            Frame();
            if (!Begun.Contains(book.Key)) waited++;
        }
        Assert.InRange(waited * 16, 240, 272);
        Assert.Contains(book.Key, Begun);
    }

    [Fact]
    public void A_target_not_there_at_all_is_not_waited_for()
    {
        var book = Grimoire();
        Spells();
        book.DockTo(Handle(), DockSide.Right);
        UiEngine.Retire(_spells);  // Better Spells unloads
        Frame();
        Assert.Equal(new[] { book.Key }, Begun);
        Assert.Equal(0.75f * 1920 - 120, Of(book).X, 3);  // its own place
    }

    [Fact]
    public void A_target_that_closes_frees_the_docked_window_and_opens_beside_it_again()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        spells.Hide();
        Drag(book, 1000, 600);
        Frame();
        Release(book);
        Frame();
        Assert.Equal((1000f, 600f), (Of(book).X, Of(book).Y));
        Assert.Equal(Handle(), book.DockedTo);
        spells.Show();
        Frame();
        // the window on the screen stays where the player has it; the one that opens goes beside it (Fuzen's WindowPair)
        Assert.Equal((1000f, 600f), (Of(book).X, Of(book).Y));
        AssertRightOf(spells, book);
        Assert.Equal(new[] { book.Key, spells.Key }, Begun);
    }

    [Fact]
    public void A_target_that_opens_beside_a_shown_window_takes_the_side_that_fits()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        spells.Hide();
        Drag(book, 10, 600);  // at the left edge: Better Spells (300 wide) has no room on its left
        Frame();
        Release(book);
        Frame();
        spells.Show();
        Frame();
        Win t = Of(spells), d = Of(book);
        Assert.Equal((10f, 600f), (d.X, d.Y));  // stays
        Assert.Equal(d.X + d.W, t.X, 3);         // Better Spells on the other side: right of Grimoire
        Assert.Equal(d.Y, t.Y, 3);
        Assert.Equal(Handle(), book.DockedTo);   // the dock the mod asked for
    }

    [Fact]
    public void A_docked_window_opened_again_after_every_window_closed_waits_for_its_target()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        spells.Hide();
        book.Hide();
        Frame();  // nothing open: Kiln.Ui draws no window (kiln.ui may not even call it)
        Frame();
        book.Show();  // one key press for both, Grimoire's binding first
        Frame();
        Assert.DoesNotContain(book.Key, Begun);  // not at its own place first
        spells.Show();
        Frame();
        Assert.Equal(new[] { spells.Key, book.Key }, Begun);
        AssertRightOf(spells, book);
    }

    [Fact]
    public void A_pair_opened_again_tries_the_docked_window_s_own_side_first()
    {
        var spells = Window(_spells, "Better Spells", new UiWindowOptions { Width = 300, Height = 200, Position = (0.95f, 0.5f), AcceptDock = true, PlacementId = "spells" });
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        Assert.Equal(Of(spells).X - Of(book).W, Of(book).X, 3);  // no room on the right: on the left
        spells.Hide();
        book.Hide();
        Frame();
        spells.MoveTo(0.3f, 0.5f);  // room on its right now
        spells.Show();
        book.Show();
        Frame();
        AssertRightOf(spells, book);
    }

    [Fact]
    public void An_unloaded_target_comes_back_with_its_reloaded_mod()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        var h = book.DockedTo!;
        spells.Dispose();  // the mod unloads (or removes the window)
        Assert.False(h.Available);
        Assert.False(h.Open);
        Assert.Null(h.Placement);
        Frame();
        Assert.Equal(h, book.DockedTo);
        var again = Window(_spells, "Better Spells", new UiWindowOptions { Width = 300, Height = 200, Position = (0.1f, 0.8f), AcceptDock = true, PlacementId = "spells" });
        Assert.True(h.Available);
        Frame();
        AssertRightOf(again, book);
    }

    [Fact]
    public void A_window_that_takes_the_target_s_id_without_AcceptDock_is_not_docked_to()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        spells.Dispose();
        Window(_spells, "Better Spells", new UiWindowOptions { Width = 300, Height = 200, Position = (0.1f, 0.8f), PlacementId = "spells" });
        var (x, y) = (Of(book).X, Of(book).Y);
        Frame();
        Assert.Equal((x, y), (Of(book).X, Of(book).Y));
    }

    [Fact]
    public void The_docked_window_s_mod_unloading_leaves_the_target_alone()
    {
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        var (x, y) = (Of(spells).X, Of(spells).Y);
        UiEngine.Retire(_grimoire);
        Frame();
        Assert.Equal(new[] { spells.Key }, Begun);
        Assert.Equal((x, y), (Of(spells).X, Of(spells).Y));
    }

    [Fact]
    public void A_loop_that_appears_later_is_cut_and_never_followed()
    {
        var spells = Spells();
        var book = Window(_grimoire, "Grimoire", new UiWindowOptions { Width = 240, Height = 160, AcceptDock = true, PlacementId = "grimoire" });
        book.DockTo(Handle(), DockSide.Right);
        // a loop DockTo could not see (the handles resolve as mods come and go): set behind its back
        spells.Dock.SetForTests(Ui.FindWindow("grimoire", "grimoire")!, DockSide.Bottom, 0);
        Frame();
        Assert.Equal(2, Begun.Count);
        AssertRightOf(spells, book);  // the older dock holds, the newer one is left out
    }

    [Fact]
    public void Two_windows_docked_to_one_target_and_a_chain_follow_it_in_one_frame()
    {
        var spells = Spells();
        var book = Window(_grimoire, "Grimoire", new UiWindowOptions { Width = 240, Height = 160, AcceptDock = true, PlacementId = "grimoire" });
        var notes = Window(_grimoire, "Notes", new UiWindowOptions { Width = 100, Height = 50 });
        var log = Window(_grimoire, "Log", new UiWindowOptions { Width = 100, Height = 50 });
        book.DockTo(Handle(), DockSide.Right);
        notes.DockTo(Ui.FindWindow("grimoire", "grimoire")!, DockSide.Bottom);  // under Grimoire
        log.DockTo(Handle(), DockSide.Left);
        Frame();
        Drag(spells, 500, 300);
        Frame();
        AssertRightOf(spells, book);
        At(Of(notes), Of(book).X, Of(book).Y + Of(book).H);
        At(Of(log), 500 - 100, 300);
    }

    // ---- what is remembered ----

    [Fact]
    public void A_remembering_docked_window_keeps_the_player_s_own_place_not_the_dock_s()
    {
        var spells = Spells(remember: true);
        var book = Grimoire(remember: true);
        Frame();
        Drag(book, 1500, 800);  // the player's own place for Grimoire
        Frame();
        Release(book);
        Frame();
        UiPlacements.Flush();
        var own = UiPlacements.Get("grimoire", true, "grimoire")!.Value;
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        Drag(book, 700, 400);  // the pair moved by Grimoire
        Frame();
        Release(book);
        Frame();
        UiPlacements.Flush();
        var kept = UiPlacements.Get("grimoire", true, "grimoire")!.Value;
        Assert.Equal(own.X, kept.X, 4);
        Assert.Equal(own.Y, kept.Y, 4);
        var root = UiPlacements.Get("betterspells", true, "spells")!.Value;  // the target remembers where the pair went
        Assert.Equal((Of(spells).X + 150) / 1920, root.X, 4);
        // opened alone, Grimoire goes back to the player's own place
        spells.Hide();
        book.Hide();
        Frame();
        book.Show();  // its target is closed: it waits a moment for it, then opens at its own place
        for (var i = 0; i < 30; i++) Frame();
        Assert.Equal(1500, Of(book).X, 2);
    }

    [Fact]
    public void A_remembering_window_undocked_by_its_mod_keeps_the_player_s_own_place_with_no_dock_left_anywhere()
    {
        Spells(remember: true);
        var book = Grimoire(remember: true);
        Frame();
        Drag(book, 1500, 800);
        Frame();
        Release(book);
        Frame();
        UiPlacements.Flush();
        var own = UiPlacements.Get("grimoire", true, "grimoire")!.Value;
        book.DockTo(Handle(), DockSide.Right);
        Frame();  // beside Better Spells: a place from the dock
        book.Undock();  // no dock left on any open window
        Frame();
        Frame();
        UiPlacements.Flush();
        var kept = UiPlacements.Get("grimoire", true, "grimoire")!.Value;
        Assert.Equal(own.X, kept.X, 4);
        Assert.Equal(own.Y, kept.Y, 4);
        Drag(book, 900, 100);  // the player places it alone: that is remembered
        Frame();
        Release(book);
        Frame();
        UiPlacements.Flush();
        Assert.Equal((900 + 120f) / 1920, UiPlacements.Get("grimoire", true, "grimoire")!.Value.X, 4);
    }

    [Fact]
    public void A_remembering_docked_window_still_remembers_its_size()
    {
        Spells();
        var book = Grimoire(remember: true);
        Frame();
        UiPlacements.Flush();
        var first = UiPlacements.Get("grimoire", true, "grimoire")!.Value;
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        var d = Of(book);
        d.Resizing = true;
        d.ResizeTo = (d.X, d.Y, 400, 300);
        Frame();
        d.Resizing = false;
        Frame();
        UiPlacements.Flush();
        var kept = UiPlacements.Get("grimoire", true, "grimoire")!.Value;
        Assert.Equal((400f, 300f), (kept.Width, kept.Height));
        Assert.Equal((first.X, first.Y), (kept.X, kept.Y));
    }

    // ---- UiDraw: the player's drag and resize this frame, modifier keys ----

    [Fact]
    public void WindowMovedByUser_and_WindowResizedByUser_read_the_current_window()
    {
        var ui = UiEngine.DrawContext;
        _currentFlags = UiNative.WindowExists | UiNative.WindowMoving;
        Assert.True(ui.WindowMovedByUser);
        Assert.False(ui.WindowResizedByUser);
        _currentFlags = UiNative.WindowExists | UiNative.WindowResizing;
        Assert.False(ui.WindowMovedByUser);
        Assert.True(ui.WindowResizedByUser);
        _currentFlags = 0;
        Assert.False(ui.WindowMovedByUser);
        UiNative.WindowStateOverride = null;  // an older kiln-ui.dll: never
        Assert.False(ui.WindowMovedByUser);
        Assert.False(ui.WindowResizedByUser);
    }

    [Fact]
    public void HeldMods_is_ImGui_s_modifier_state_of_this_frame()
    {
        _mods = (uint)(KeyMods.Shift | KeyMods.Ctrl);
        Assert.Equal(KeyMods.Shift | KeyMods.Ctrl, UiEngine.DrawContext.HeldMods);
        _mods = (uint)KeyMods.Alt;
        Assert.Equal(KeyMods.Alt, UiEngine.DrawContext.HeldMods);
    }

    [Fact]
    public void The_window_state_slot_and_record_match_kiln_ui_h()
    {
        Assert.Equal(1096, sizeof(UiNative.TableWindowState));  // window_state at 1088, after popups
        Assert.Equal(1088, (int)Marshal.OffsetOf<UiNative.TableWindowState>(nameof(UiNative.TableWindowState.WindowStateFn)));
        Assert.Equal(24, sizeof(UiNative.WindowState));
        Assert.Equal((1u, 2u, 4u), (UiNative.WindowExists, UiNative.WindowMoving, UiNative.WindowResizing));
    }

    [Fact]
    public void Without_window_state_a_docked_window_still_follows_its_target()
    {
        UiNative.WindowStateOverride = null;  // an older kiln-ui.dll: no drag or resize is known
        var spells = Spells();
        var book = Grimoire();
        book.DockTo(Handle(), DockSide.Right);
        Frame();
        spells.MoveTo(0.5f, 0.5f, 0, 0);
        Frame();
        AssertRightOf(spells, book);
    }
}
