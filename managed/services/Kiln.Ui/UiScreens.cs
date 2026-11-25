// SPDX-License-Identifier: MIT
// Custom full screens for mods (Forge: a Screen subclass + Minecraft.setScreen). Drawn with Kiln's ImGui overlay in
// the player's look (UiStyle.Player: the game's colours and its own UI font, read at run time, unless the player chose
// another look), the game's (UiStyle.Game), Kiln's (UiStyle.Kiln), or a mod's own theme (Theme).
// While a screen is open it has the mouse and keyboard (the game's input pauses); Escape closes the top screen.
namespace Kiln;

/// <summary>The look of a Kiln window or screen.</summary>
public enum UiStyle
{
    /// <summary>The game's menu colours and its own UI font (read from the game's resources at run time).</summary>
    Game = 1,
    /// <summary>Kiln's ember theme (branding/palette.json).</summary>
    Kiln = 0,
    /// <summary>The look the player chose for Kiln's UI (Kiln settings > Look, kiln.json <c>ui</c>): the game's unless they
    /// changed it. The default of windows, screens and crafting panels since 0.2.0-dev.29.16.</summary>
    Player = 2,
}

/// <summary>Options of a <see cref="UiScreen"/>.</summary>
public sealed record UiScreenOptions
{
    /// <summary>The screen's look; <see cref="UiStyle.Player"/> by default (the game's look unless the player chose another;
    /// before 0.2.0-dev.29.16 the default was <see cref="UiStyle.Game"/>).</summary>
    public UiStyle Style { get; init; } = UiStyle.Player;

    /// <summary>Your own theme for the screen (<see cref="Ui.Themes.Define"/>), over <see cref="Style"/>. Since 0.2.0-dev.29.16.</summary>
    public UiTheme? Theme { get; init; }

    /// <summary>Size in pixels at UI scale 1, centred on the screen (or at <see cref="Position"/>). Default 720.</summary>
    public float Width { get; init; } = 720;
    /// <summary>Size in pixels at UI scale 1, centred on the screen (or at <see cref="Position"/>). Default 520.</summary>
    public float Height { get; init; } = 520;

    /// <summary>Where the screen opens: its centre, as a fraction of the viewport (0..1, as
    /// <see cref="UiWindowOptions.Position"/>); null = the middle of the screen. Applied each time the screen opens; the
    /// whole screen is kept inside the viewport. A remembered placement (<see cref="RememberPlacement"/>) and
    /// <see cref="UiScreen.MoveTo"/> win over it. Ignored by <see cref="FullScreen"/>.</summary>
    /// <remarks>Since 0.2.0-dev.31.11, mod API 0.2.20.</remarks>
    public (float X, float Y)? Position { get; init; }

    /// <summary>Kiln remembers where the player left the screen (and its size, when <see cref="Resizable"/>), per mod and
    /// <see cref="PlacementId"/>, across opens and game starts (ui-placement.json in Kiln's config folder), and opens it
    /// there, kept inside the current viewport. It wins over <see cref="Position"/>, <see cref="Width"/> and
    /// <see cref="Height"/>; <see cref="UiScreen.MoveTo"/> and <see cref="UiScreen.Resize"/> win over it. Default false.
    /// Ignored by <see cref="FullScreen"/>.</summary>
    /// <remarks>Since 0.2.0-dev.31.11, mod API 0.2.20.</remarks>
    public bool RememberPlacement { get; init; }

    /// <summary>The id the remembered placement is kept under, unique among your mod's screens; null = the title in English
    /// (<see cref="UiText.For"/> "en"). Give one when the title changes (a count in it, a rename).</summary>
    /// <remarks>Since 0.2.0-dev.31.11, mod API 0.2.20.</remarks>
    public string? PlacementId { get; init; }
    /// <summary>Escape closes the screen (the top one first).</summary>
    public bool CloseOnEscape { get; init; } = true;
    /// <summary>Darken the game behind the screen.</summary>
    public bool DimBackground { get; init; } = true;
    /// <summary>Whether the screen has a title bar. Default true.</summary>
    public bool TitleBar { get; init; } = true;
    /// <summary>Whether the player can resize the screen. Default false.</summary>
    public bool Resizable { get; init; }
    /// <summary>A close button (X) in the title bar.</summary>
    public bool CloseButton { get; init; } = true;

    /// <summary>The whole display: no title bar, not movable or resizable, no frame (Width and Height are ignored). Draw
    /// your own title and layout; <see cref="UiDraw.Width"/> / <see cref="UiDraw.Height"/> are the display's size.
    /// Escape still closes it.</summary>
    public bool FullScreen { get; init; }
}

/// <summary>A full screen of a mod: a menu with lists, buttons, sliders, text fields and images. Open it from a
/// <see cref="Ui.Menus"/> entry, a key binding or a command (any thread). While open it takes the mouse and keyboard
/// (the game's input pauses; key releases still reach the game), and Escape closes it.</summary>
/// <example><code>
/// var screen = Ui.Screen("My mod", ui => { ui.Text("Hello"); if (ui.Button("Close")) ui.CloseScreen(); });
/// Menus.Add(MenuLocation.PauseMenu, "My mod", screen.Open);
/// KeyBindings.Register("mymod.screen", "F7", screen.Toggle);
/// </code></example>
public sealed class UiScreen : IDisposable
{
    private static int _ids;
    private volatile bool _open;

    internal UiScreen(UiText title, Action<UiDraw> draw, UiScreenOptions options, ModContext? owner)
    {
        Title = title;
        Draw = draw;
        Options = options;
        Owner = owner;
        Id = $"###kiln.screen{Interlocked.Increment(ref _ids)}";
        Placer = UiPlacer.For(owner, options.RememberPlacement && !options.FullScreen, options.PlacementId, () => title.For("en"), window: false);
    }

    /// <summary>The title shown in the title bar.</summary>
    public UiText Title { get; set; }
    /// <summary>The options the screen was created with.</summary>
    public UiScreenOptions Options { get; }
    internal Action<UiDraw> Draw { get; }
    internal ModContext? Owner { get; }
    internal string Id { get; }
    internal bool Disposed;
    internal long OpenedAt;
    internal bool FocusNext;
    internal UiPlacer Placer { get; }

    /// <summary>The screen is open now.</summary>
    public bool IsOpen => _open;

    /// <summary>Runs (as the owning mod, on the thread that closed it) when the screen closes.</summary>
    public event Action? Closed;

    /// <summary>Runs when the screen opens.</summary>
    public event Action? Opened;

    /// <summary>Opens the screen (does nothing when it is open, removed, or the overlay does not draw).</summary>
    public void Open()
    {
        if (Disposed || _open || UiNative.DrawingOffReason != null) return;
        Placer.Opened();  // before _open: the render thread's first frame of this open is always its appearing one
        _open = true;
        OpenedAt = Interlocked.Increment(ref UiEngine.ScreenOrder);
        FocusNext = true;
        UiEngine.Recount();
        if (Opened is { } o) UiModCalls.Invoke(Owner, static a => a(), o, "screen Opened");
    }

    /// <summary>Closes the screen, if it is open.</summary>
    public void Close()
    {
        if (!_open) return;
        _open = false;
        UiEngine.Recount();
        if (Closed is { } c) UiModCalls.Invoke(Owner, static a => a(), c, "screen Closed");
    }

    /// <summary>Opens the screen if it is closed, closes it if it is open.</summary>
    public void Toggle()
    {
        if (_open) Close();
        else Open();
    }

    /// <summary>Moves the screen: the point (<paramref name="pivotX"/>, <paramref name="pivotY"/>) of the screen (0.5, 0.5 =
    /// its centre; 0, 0 = its top left corner) goes to (<paramref name="x"/>, <paramref name="y"/>), fractions of the
    /// viewport (0..1), the whole screen kept inside it. Applied once, on the next frame the screen is drawn: on an open
    /// screen the next frame, on a closed one the first frame after it next opens (a later call replaces one not yet
    /// applied). It wins over the remembered placement and <see cref="UiScreenOptions.Position"/> for that frame; after it
    /// the player can drag the screen again, and the next open starts from the remembered placement or the options. Does
    /// nothing on a removed or <see cref="UiScreenOptions.FullScreen"/> screen. Any thread.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A position that is not a finite number, or a pivot outside 0..1.</exception>
    /// <remarks>Since 0.2.0-dev.31.11, mod API 0.2.20.</remarks>
    [AnyThread]
    public void MoveTo(float x, float y, float pivotX = 0.5f, float pivotY = 0.5f)
    {
        UiPlacer.CheckMove(x, y, pivotX, pivotY);
        if (Disposed || Options.FullScreen) return;
        Placer.Move(x, y, pivotX, pivotY);
    }

    /// <summary>Resizes the screen to <paramref name="width"/> x <paramref name="height"/> pixels at UI scale 1 (the units of
    /// <see cref="UiScreenOptions.Width"/>), at most the viewport; ImGui's minimum window size still applies. Applied once,
    /// like <see cref="MoveTo"/> (also on a closed screen: when it next opens); a resize alone keeps the screen's top left
    /// corner, moved back in when the new size would cross the viewport's edge. Works whether or not the player can
    /// resize it (<see cref="UiScreenOptions.Resizable"/>). Does nothing on a removed or full-screen screen. Any thread.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A size that is not a finite number above 0.</exception>
    /// <remarks>Since 0.2.0-dev.31.11, mod API 0.2.20.</remarks>
    [AnyThread]
    public void Resize(float width, float height)
    {
        UiPlacer.CheckSize(width, height);
        if (Disposed || Options.FullScreen) return;
        Placer.Resize(width, height);
    }

    /// <summary>Where the screen was drawn last (fractions of the viewport, sizes at UI scale 1); null before its first
    /// frame, and always for a <see cref="UiScreenOptions.FullScreen"/> screen (it is not placed). Kept while it is closed. Hand it to another mod (<c>Mods.Send</c>) to open a screen beside this one. Any
    /// thread.</summary>
    /// <remarks>Since 0.2.0-dev.31.11, mod API 0.2.20.</remarks>
    [AnyThread]
    public UiPlacement? Placement => Placer.Placement;

    /// <summary>Removes the screen (also done when the owning mod unloads).</summary>
    public void Dispose()
    {
        Close();
        UiEngine.RemoveScreen(this);
    }
}

public static partial class Ui
{
    /// <summary>A full screen drawn by <paramref name="draw"/> every frame while open (closed at first). Owned by the
    /// calling mod (removed when it unloads).</summary>
    public static UiScreen Screen(UiText title, Action<UiDraw> draw, UiScreenOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(title);
        ArgumentNullException.ThrowIfNull(draw);
        UiEngine.EnsureStarted();
        return UiEngine.AddScreen(new UiScreen(title, draw, options ?? new UiScreenOptions(), UiModCalls.Caller));
    }

    /// <summary>The screens open now, the top one last.</summary>
    public static IReadOnlyList<UiScreen> OpenScreens => UiEngine.OpenScreens();
}
