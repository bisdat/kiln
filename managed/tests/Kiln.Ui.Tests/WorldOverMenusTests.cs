// SPDX-License-Identifier: MIT
// GitHub issue #138 (1), 0.2.0-dev.31: RenderFrameEvent.World submits nothing while a known game menu covers the world
// (map, inventory, crafting, ...), WorldOverMenus does; on the HUD, an unknown screen or no screen state both draw as before.
// Projection, Ready and the return values are the same in every case; pixel drawing (Draw) is never hidden. A fake
// kiln.render table counts the primitives that reach the native side; a fake kiln.ui answers get_screen.
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace WorldOverMenusTestSuite;

internal static unsafe class CountingRender
{
    public static RenderNative.Table* Table;
    public static int Lines, Shapes, Texts, Images, Rects;

    public static void Install()
    {
        Free();
        Table = (RenderNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(RenderNative.Table));
        Table->Header = new ServiceHeader { Size = (uint)sizeof(RenderNative.Table), Version = 1 };
        Table->GetStatus = &Status;
        Table->Line = &Line;
        Table->Rect = &Rect;
        Table->Circle = &Circle;
        Table->Triangle = &Triangle;
        Table->Polyline = &Polyline;
        Table->Text = &Text;
        Table->Image = &Image;
        Clear();
    }

    public static void Clear() => Lines = Shapes = Texts = Images = Rects = 0;
    public static int All => Lines + Shapes + Texts + Images + Rects;

    public static void Free()
    {
        if (Table != null) NativeMemory.Free(Table);
        Table = null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(RenderNative.Status* s) => 3;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Line(float a, float b, float c, float d, uint rgba, float t) => Lines++;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Rect(float a, float b, float c, float d, uint rgba, float r, float t, int fill) => Rects++;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Circle(float x, float y, float r, uint rgba, int seg, float t, int fill) => Shapes++;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Triangle(float a, float b, float c, float d, float e, float f, uint rgba, float t, int fill) => Shapes++;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Polyline(float* p, uint n, uint rgba, float t, int closed, int fill) => Shapes++;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Text(float x, float y, uint rgba, float size, uint flags, NativeStr s) => Texts++;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Image(ulong h, float a, float b, float c, float d, float e, float f, float g, float i, uint rgba) => Images++;

    public sealed class Backend : IKilnBackend
    {
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public void Log(ulong owner, LogLevel level, string text) { }
        public nint Service(string name, uint minVersion) =>
            name == "kiln.render" ? (nint)Table : name == "kiln.ui" && FakeUiTable.Table != null ? (nint)FakeUiTable.Table : 0;
        public ulong OwnerCreate(string id) => 99;
    }
}

[Collection(UiSerial.Name)]
public sealed unsafe class WorldOverMenusTests : IDisposable
{
    private const float W = 1920, H = 1080;
    private static readonly WorldPosition Ahead = new(0, 0, 10);
    private readonly IKilnBackend _previous;
    private static UiNative.Screen _screen;
    private static int _screenRc;

    public WorldOverMenusTests()
    {
        _previous = Backend.Current;
        FakeUiTable.Install();
        CountingRender.Install();
        Backend.Current = new CountingRender.Backend();
        RenderNative.ResetForTests();
        UiNative.ResetForTests();
        FakeUiTable.Table->Base.Base.Base.GetScreen = &GetScreen;
        _screen = default;
        _screenRc = 0;
    }

    public void Dispose()
    {
        Backend.Current = _previous;
        RenderNative.ResetForTests();
        UiNative.ResetForTests();
        CountingRender.Free();
        FakeUiTable.Free();
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetScreen(UiNative.Screen* s)
    {
        if (_screenRc != 0) return _screenRc;
        var size = s->Size;
        *s = _screen;
        s->Size = size;
        return 0;
    }

    private static GameScreen Screen(GameUiState state, HudScreen hud) =>
        new(state, hud, GameScreen.KindOf(state, hud, 0), 0, null, 0, 1);

    private static RenderFrameEvent Frame(GameScreen? screen) => new()
    {
        Width = W, Height = H, Scale = 1, Camera = new RenderCamera(new WorldPosition(0, 0, 0), Quaternion.Identity, MathF.PI / 3),
        ScreenSnapshot = screen,
    };

    /// <summary>Every WorldDraw primitive once, inside the handler's drawing window; the return values of the bool ones.</summary>
    private static (int Submitted, bool[] Returned) DrawAll(RenderFrameEvent ev, WorldDraw view)
    {
        CountingRender.Clear();
        ev.Draw.Begin();
        try
        {
            var image = new UiImage { Handle = 5, Width = 32, Height = 32 };
            bool[] r =
            [
                view.Marker(Ahead, Color.Red),
                view.Marker(Ahead, Color.Red, shape: MarkerShape.Circle),
                view.Label(Ahead, "here"),
                view.Icon(Ahead, image),
                view.ToScreen(Ahead, out _),
                view.Ready,
            ];
            view.Line(new WorldPosition(-1, 0, 10), new WorldPosition(1, 0, 10), Color.Green);
            view.Box(new WorldPosition(-1, -1, 9), new WorldPosition(1, 1, 11), Color.Green);
            view.Circle(Ahead, 2, Color.Green);
            return (CountingRender.All, r);
        }
        finally
        {
            ev.Draw.End();
        }
    }

    public static TheoryData<GameUiState, HudScreen> Menus => new()
    {
        { GameUiState.Play, HudScreen.Map },
        { GameUiState.Play, HudScreen.InGameMenu },
        { GameUiState.Play, HudScreen.Backpack },
        { GameUiState.Play, HudScreen.PlayerCrafting },
        { GameUiState.Play, HudScreen.StationBasedCrafting },
        { GameUiState.Play, HudScreen.FactoryCrafting },
        { GameUiState.Play, HudScreen.StorageChest },
        { GameUiState.Play, HudScreen.Character },
        { GameUiState.Play, HudScreen.Journal },
        { GameUiState.Play, HudScreen.SkillTree },
        { GameUiState.Play, HudScreen.NpcDialog },
        { GameUiState.Play, HudScreen.SystemMenu },
        { GameUiState.Play, HudScreen.SettingsMenu },
        { GameUiState.PreGame, HudScreen.Invalid },
        { GameUiState.Load, HudScreen.Invalid },
    };

    [Theory]
    [MemberData(nameof(Menus))]
    public void World_submits_nothing_over_a_known_game_menu_and_WorldOverMenus_still_draws(GameUiState state, HudScreen hud)
    {
        var ev = Frame(null);
        var (hudCount, hudReturns) = DrawAll(ev, ev.World);
        Assert.True(hudCount > 0);

        ev.ScreenSnapshot = Screen(state, hud);
        Assert.True(WorldDraw.HiddenOn(ev.Screen.Kind), $"{state}/{hud} is {ev.Screen.Kind}");
        var (hidden, hiddenReturns) = DrawAll(ev, ev.World);
        Assert.Equal(0, hidden);
        Assert.Equal(hudReturns, hiddenReturns);  // the projection and the answers do not change

        var (over, overReturns) = DrawAll(ev, ev.WorldOverMenus);
        Assert.Equal(hudCount, over);
        Assert.Equal(hudReturns, overReturns);
    }

    public static TheoryData<GameUiState, HudScreen> Drawn => new()
    {
        { GameUiState.Play, HudScreen.InGame },
        { GameUiState.Play, HudScreen.NoUi },
        { GameUiState.Play, HudScreen.Invalid },
        { GameUiState.Play, HudScreen.TextChat },
        { GameUiState.None, HudScreen.Invalid },
        { GameUiState.Play, (HudScreen)250 },  // a screen Kiln does not know
    };

    [Theory]
    [MemberData(nameof(Drawn))]
    public void The_hud_and_unknown_screens_draw_as_before(GameUiState state, HudScreen hud)
    {
        var plain = Frame(null);
        var (expected, returns) = DrawAll(plain, plain.World);
        var ev = Frame(Screen(state, hud));
        Assert.False(WorldDraw.HiddenOn(ev.Screen.Kind));
        var (count, r) = DrawAll(ev, ev.World);
        Assert.Equal(expected, count);
        Assert.Equal(returns, r);
        Assert.Equal(expected, DrawAll(ev, ev.WorldOverMenus).Submitted);
    }

    [Fact]
    public void No_screen_state_draws_as_before()
    {
        var ev = Frame(null);
        Assert.False(ev.OverGameMenu);
        Assert.True(DrawAll(ev, ev.World).Submitted > 0);
    }

    [Fact]
    public void Pixel_drawing_is_never_hidden()
    {
        var ev = Frame(Screen(GameUiState.Play, HudScreen.Map));
        CountingRender.Clear();
        ev.Draw.Begin();
        ev.Draw.Line(0, 0, 10, 10, Color.Red);
        ev.Draw.Text(5, 5, "fps");
        ev.Draw.FillRect(0, 0, 4, 4, Color.Red);
        ev.Draw.End();
        Assert.Equal(3, CountingRender.All);
    }

    [Fact]
    public void Projection_is_the_same_over_menus()
    {
        var hud = Frame(Screen(GameUiState.Play, HudScreen.InGame));
        var map = Frame(Screen(GameUiState.Play, HudScreen.Map));
        Assert.True(hud.World.ToScreen(new WorldPosition(1, 2, 10), out var a));
        Assert.True(map.World.ToScreen(new WorldPosition(1, 2, 10), out var b));
        Assert.True(map.WorldOverMenus.ToScreen(new WorldPosition(1, 2, 10), out var c));
        Assert.Equal(a, b);
        Assert.Equal(a, c);
        Assert.True(map.World.Ready);
        Assert.False(map.World.ToScreenClamped(new WorldPosition(0, 0, -10), 20, out _, out _));
    }

    [Fact]
    public void The_frame_reads_the_game_screen_once_and_unknown_state_is_null()
    {
        _screen = new UiNative.Screen { UiState = (byte)GameUiState.Play, HudScreen = (byte)HudScreen.Map, Frame = 7 };
        Assert.True(Ui.Screens.TryRead(out var s));
        Assert.Equal(ScreenKind.Map, s.Kind);
        _screenRc = 3;  // kiln.ui could not read it: not known
        Assert.False(Ui.Screens.TryRead(out s));
        Assert.Equal(GameScreen.None, s);
    }
}
