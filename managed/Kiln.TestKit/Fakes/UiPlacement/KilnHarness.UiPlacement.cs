// SPDX-License-Identifier: MIT
// Where a mod's screens and windows sit, offline (GitHub issue 146, since 0.2.0-dev.31.11, mod API 0.2.20): Kiln.Ui's own
// placement code (UiScreenOptions.Position, MoveTo, Resize, Placement, RememberPlacement and ui-placement.json in the
// harness's config folder) runs unchanged; the kit plays the overlay's frames on a viewport the test sets, with ImGui's
// conditions (Always, FirstUseEver, Appearing) and its 32-pixel minimum window size modelled. kiln.ui itself stays offline:
// nothing is drawn, Ui.Available is false. Window docking (GitHub issue 149): FrameWindows plays a frame of every open window
// with Kiln.Ui's own dock code (UiDock.cs), PlayerDrags is the player holding and dragging a window.
using System.Runtime.CompilerServices;

namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessUiPlacement? _uiPlacement;

    /// <summary>Where a mod's screens and windows sit: play an overlay frame for one (<see cref="HarnessUiPlacement.Frame(UiScreen)"/>),
    /// move it as the player would, read what Kiln remembers. Since 0.2.0-dev.31.11, mod API 0.2.20.</summary>
    public HarnessUiPlacement UiPlacement => _uiPlacement ??= new HarnessUiPlacement(this);
}

/// <summary>
/// The overlay's frames for a mod's <see cref="UiScreen"/> and <see cref="UiWindow"/> placement (<see cref="KilnHarness.UiPlacement"/>):
/// Kiln.Ui's own code decides where each goes. Since 0.2.0-dev.31.11 (mod API 0.2.20).
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> One <see cref="Frame(UiScreen)"/> is one overlay frame of that screen or window: Kiln.Ui asks for
/// its size and place as in the game (the options, the remembered placement, a waiting MoveTo / Resize, clamping into the
/// viewport), and the kit applies the ask as ImGui would (Always; FirstUseEver on the first frame ever; Appearing on the first
/// frame after it was not drawn; a minimum of 32 pixels; an AutoResize window takes <see cref="ContentSize"/>). The rect is
/// what <see cref="UiScreen.Placement"/> reads after; a remembering screen or window writes ui-placement.json in the
/// harness's config folder at once (in a game: about a second after the change). <see cref="PlayerPlaces(UiScreen, float, float, float, float)"/>
/// is the player's drag or resize in the next frame; <see cref="Restart"/> is a game start (the file is read again).</para>
/// <para><b>Docking</b> (<see cref="UiWindow.DockTo"/>, since 0.2.0-dev.31.next). <see cref="FrameWindows"/> is one overlay frame
/// of every open window, in the order and with the places Kiln.Ui's own dock code gives them (the group's leader first, the
/// others beside it in the same frame, kept inside the viewport, a docked window waiting for its closed target, Shift, a drop
/// beside the old side): use it, not <see cref="Frame(UiWindow)"/>, for docked windows. <see cref="PlayerDrags"/> is the player
/// holding a window and dragging it in the next frame (the frame after without another one is the drop); a
/// <see cref="PlayerPlaces(UiWindow, float, float, float, float)"/> that changes the size is a resize. The kit's clock moves
/// 16 ms a frame (a docked window waits for its target at most 250 ms: 16 frames).</para>
/// <para><b>Not modelled.</b> kiln.ui itself (no window is drawn, Ui.Available is false), ImGui's own saved places
/// (imgui.ini), size constraints a native plugin sets, the title bar, scrolling and the frame's timing.</para>
/// </remarks>
public sealed class HarnessUiPlacement
{
    private const float MinSize = 32, DefaultX = 60, DefaultY = 60;

    private readonly ConditionalWeakTable<object, Window> _windows = new();

    internal HarnessUiPlacement(KilnHarness harness) => ArgumentNullException.ThrowIfNull(harness);

    private sealed class Window
    {
        public (float X, float Y, float W, float H)? Rect;
        public (float X, float Y, float W, float H)? Player;
        public bool Ever, Shown;
        public long Opens;
        // the player holds it and has dragged its centre here (fractions), for the next FrameWindows
        public (float X, float Y)? Drag;
    }

    private bool _shift;
    private long _clock = 1_000_000;

    /// <summary>The display's width in pixels (1920 unless the test says).</summary>
    public float ViewportWidth { get; set; } = 1920;

    /// <summary>The display's height in pixels (1080 unless the test says).</summary>
    public float ViewportHeight { get; set; } = 1080;

    /// <summary>The UI scale (1 unless the test says): sizes at UI scale 1 are multiplied by it.</summary>
    public float Scale { get; set; } = 1;

    /// <summary>The size, in pixels at UI scale 1, that a window sized by its contents takes (Width or Height 0, or
    /// AutoResize). 320 x 240 unless the test says.</summary>
    public (float Width, float Height) ContentSize { get; set; } = (320, 240);

    /// <summary>ui-placement.json in the harness's config folder (it exists once something was remembered).</summary>
    public string? FilePath => UiPlacements.FilePath;

    private UiViewport Viewport => new(ViewportWidth, ViewportHeight, Scale);

    /// <summary>One overlay frame of <paramref name="screen"/>: where it is now (fractions of the viewport, sizes at UI
    /// scale 1), or null when it is closed or removed (its waiting MoveTo / Resize wait on) or <see cref="UiScreenOptions.FullScreen"/>.</summary>
    public UiPlacement? Frame(UiScreen screen)
    {
        ArgumentNullException.ThrowIfNull(screen);
        var w = _windows.GetOrCreateValue(screen);
        if (!screen.IsOpen || screen.Disposed || screen.Options.FullScreen)
        {
            w.Shown = false;
            return null;
        }
        var o = screen.Options;
        var vp = Viewport;
        var plan = screen.Placer.PlanScreen(vp, o);
        Draw(w, screen.Placer, vp, plan, (o.Width * vp.Scale, o.Height * vp.Scale), autoFit: false);
        return screen.Placer.Placement;
    }

    /// <summary>One overlay frame of <paramref name="window"/>: where it is now, or null when it is hidden or removed.</summary>
    public UiPlacement? Frame(UiWindow window)
    {
        ArgumentNullException.ThrowIfNull(window);
        var w = _windows.GetOrCreateValue(window);
        if (!window.Open || window.Disposed)
        {
            w.Shown = false;
            return null;
        }
        var o = window.Options;
        var vp = Viewport;
        var plan = window.Placer.PlanWindow(vp, o);
        var content = (ContentSize.Width * vp.SafeScale, ContentSize.Height * vp.SafeScale);
        Draw(w, window.Placer, vp, plan, content, autoFit: o.AutoResize);
        return window.Placer.Placement;
    }

    /// <summary>The player drags and resizes <paramref name="screen"/> in its next frame: its centre to (<paramref name="x"/>,
    /// <paramref name="y"/>), fractions of the viewport, at <paramref name="width"/> x <paramref name="height"/> pixels at UI
    /// scale 1 (the game lets the player resize only a <see cref="UiScreenOptions.Resizable"/> screen; the kit does not check).</summary>
    public void PlayerPlaces(UiScreen screen, float x, float y, float width, float height)
    {
        ArgumentNullException.ThrowIfNull(screen);
        _windows.GetOrCreateValue(screen).Player = Pixels(x, y, width, height);
    }

    /// <summary>The player drags and resizes <paramref name="window"/> in its next frame (as for a screen).</summary>
    public void PlayerPlaces(UiWindow window, float x, float y, float width, float height)
    {
        ArgumentNullException.ThrowIfNull(window);
        _windows.GetOrCreateValue(window).Player = Pixels(x, y, width, height);
    }

    /// <summary>What Kiln remembers for <paramref name="screen"/> (null: nothing, or it does not remember). The edges are
    /// worked out for the harness's viewport.</summary>
    public UiPlacement? Remembered(UiScreen screen)
    {
        ArgumentNullException.ThrowIfNull(screen);
        return Remembered(screen.Placer);
    }

    /// <summary>What Kiln remembers for <paramref name="window"/> (null: nothing, or it does not remember).</summary>
    public UiPlacement? Remembered(UiWindow window)
    {
        ArgumentNullException.ThrowIfNull(window);
        return Remembered(window.Placer);
    }

    /// <summary>A game start for placements: what Kiln remembers is read from ui-placement.json again (write it first to
    /// script a file a player brought, or a corrupt one).</summary>
    public void Restart()
    {
        UiPlacements.Flush();
        UiPlacements.Forget();
    }

    /// <summary>One overlay frame of every open <see cref="UiWindow"/> (of every mod in the harness), docked ones together:
    /// where each one is now (fractions of the viewport, sizes at UI scale 1), keyed by window. A window that is closed,
    /// removed, or waiting for its dock's target is not in it. Since 0.2.0-dev.31.next (GitHub issue 149).</summary>
    public IReadOnlyDictionary<UiWindow, UiPlacement> FrameWindows()
    {
        var vp = Viewport;
        var open = UiEngine.Windows.Where(w => w.Open && !w.Disposed).ToArray();
        _clock += 16;
        var dock = UiDockFrame.Plan(open, vp, _shift ? KeyMods.Shift : KeyMods.None, Live, _clock);
        var drawn = new Dictionary<UiWindow, UiPlacement>(ReferenceEqualityComparer.Instance);
        try
        {
            foreach (var window in dock.Order)
            {
                var w = _windows.GetOrCreateValue(window);
                var plan = dock.Place(window, window.Placer.PlanWindow(vp, window.Options));
                var content = (ContentSize.Width * vp.SafeScale, ContentSize.Height * vp.SafeScale);
                var start = w.Drag is { } d ? DragStart(w, d, content) : ((float, float)?)null;
                var rect = Draw(w, window.Placer, vp, plan, content, window.Options.AutoResize, start, dock.KeepsPosition(window));
                dock.Drawn(window, rect.X, rect.Y, rect.W, rect.H);
                if (window.Placer.Placement is { } p) drawn[window] = p;
            }
            foreach (var window in UiEngine.Windows)
                if (!drawn.ContainsKey(window)) _windows.GetOrCreateValue(window).Shown = false;
        }
        finally
        {
            dock.Finish();
            foreach (var window in open) _windows.GetOrCreateValue(window).Drag = null;
            _shift = false;
        }
        return drawn;
    }

    /// <summary>The player holds <paramref name="window"/> and drags it in the next <see cref="FrameWindows"/>, its centre to
    /// (<paramref name="x"/>, <paramref name="y"/>), fractions of the viewport, with Shift held or not (Shift: it moves alone and
    /// leaves its dock). The frame after without another drag is the drop (a window that was undocked by Shift docks again
    /// there when it is within 16 pixels of its old side). Since 0.2.0-dev.31.next (GitHub issue 149).</summary>
    public void PlayerDrags(UiWindow window, float x, float y, bool shift = false)
    {
        ArgumentNullException.ThrowIfNull(window);
        _windows.GetOrCreateValue(window).Drag = (x, y);
        _shift |= shift;
    }

    // ImGui's NewFrame: the dragged window's top left corner from the pointer, at its size
    private (float X, float Y) DragStart(Window w, (float X, float Y) centre, (float W, float H) content)
    {
        var (sw, sh) = w.Rect is { } r ? (r.W, r.H) : content;
        return (centre.X * ViewportWidth - sw / 2, centre.Y * ViewportHeight - sh / 2);
    }

    // kiln.ui's window_state as the kit has it: where the window is before its Begin (a drag already in), dragged or resized
    private UiWindowLive Live(UiWindow window)
    {
        var w = _windows.GetOrCreateValue(window);
        if (w.Rect is not { } r) return default;
        var resizing = w.Player is { } p && (MathF.Abs(p.W - r.W) > 0.01f || MathF.Abs(p.H - r.H) > 0.01f);
        if (w.Drag is { } d)
        {
            var (x, y) = DragStart(w, d, (r.W, r.H));
            return new UiWindowLive(true, true, resizing, x, y, r.W, r.H);
        }
        return new UiWindowLive(true, false, resizing, r.X, r.Y, r.W, r.H);
    }

    private UiPlacement? Remembered(UiPlacer placer)
    {
        if (placer.Id is not { } id || UiPlacements.Get(placer.Mod, placer.IsWindow, id) is not { } e) return null;
        var vp = Viewport;
        float w = e.Width * vp.SafeScale, h = e.Height * vp.SafeScale;
        return UiPlacement.FromPixels(vp, e.X * vp.Width - w / 2, e.Y * vp.Height - h / 2, w, h);
    }

    private (float, float, float, float) Pixels(float x, float y, float width, float height)
    {
        float w = width * Viewport.SafeScale, h = height * Viewport.SafeScale;
        return (x * ViewportWidth - w / 2, y * ViewportHeight - h / 2, w, h);
    }

    // ImGui's Begin with the SetNextWindowSize / SetNextWindowPos Kiln asked for (over where a drag put it in NewFrame), then
    // the player's resize
    private (float X, float Y, float W, float H) Draw(Window w, UiPlacer placer, UiViewport vp, UiPlan plan, (float W, float H) fallback,
        bool autoFit, (float X, float Y)? start = null, bool keepPosition = false)
    {
        var appearing = !w.Shown || w.Opens != placer.Opens;  // closed and opened again since its last frame
        w.Opens = placer.Opens;
        bool Applies(uint cond) => cond == UiPlan.Always || (cond == UiPlan.Appearing && appearing) || (cond is UiPlan.FirstUseEver or 2 && !w.Ever);
        float sw, sh;
        if (autoFit) (sw, sh) = fallback;
        else if (plan.HasSize && Applies(plan.SizeCond)) (sw, sh) = (plan.W > 0 ? plan.W : fallback.W, plan.H > 0 ? plan.H : fallback.H);
        else if (w.Rect is { } r) (sw, sh) = (r.W, r.H);
        else (sw, sh) = fallback;
        if (!(sw > 0)) sw = ContentSize.Width * vp.SafeScale;
        if (!(sh > 0)) sh = ContentSize.Height * vp.SafeScale;
        (sw, sh) = (MathF.Max(sw, MinSize), MathF.Max(sh, MinSize));
        float x, y;
        if (plan.HasPos && Applies(plan.PosCond)) (x, y) = (plan.X - plan.PivotX * sw, plan.Y - plan.PivotY * sh);
        else if (start is { } s) (x, y) = s;
        else if (w.Rect is { } r) (x, y) = (r.X, r.Y);
        else (x, y) = (DefaultX, DefaultY);
        var rect = w.Player ?? (x, y, sw, sh);
        w.Player = null;
        w.Rect = rect;
        w.Ever = w.Shown = true;
        placer.Drawn(vp, rect.X, rect.Y, rect.W, rect.H, visible: true, keepPosition);
        UiPlacements.Flush();
        return rect;
    }
}
