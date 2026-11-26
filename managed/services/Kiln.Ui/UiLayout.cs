// SPDX-License-Identifier: MIT
// Layout and look for full-display screens (kiln.ui v3, kiln_ui.h "v3"): font sizes, child regions with flags, the
// cursor and the last widget's rectangle, sized selectables, keyboard focus, drawing into a window, colour and spacing
// pushes, and key queries. The Kiln mod manager is built with them; mods may use them too. On an older kiln.ui each
// call falls back to the nearest older one or does nothing (HasLayout says which).
namespace Kiln;

/// <summary>Flags of <see cref="UiDraw.BeginChild(string, float, float, UiChildFlags, UiWindowFlags)"/>.</summary>
[Flags]
public enum UiChildFlags : uint
{
    /// <summary>No flags.</summary>
    None = 0,
    /// <summary>An outline (and the window padding).</summary>
    Border = 1u << 0,
    /// <summary>The window padding without an outline.</summary>
    Padding = 1u << 1,
    /// <summary>As tall as its contents (pass height 0).</summary>
    AutoHeight = 1u << 5,
    /// <summary>Looks like a frame (frame colours and padding).</summary>
    FrameStyle = 1u << 7,
    /// <summary>Gamepad and keyboard navigation moves into and out of it like plain widgets (use it for columns).</summary>
    NavFlattened = 1u << 8,
}

/// <summary>Window flags for child regions.</summary>
[Flags]
public enum UiWindowFlags : uint
{
    /// <summary>No flags.</summary>
    None = 0,
    /// <summary>No scrollbar (the region still scrolls with the mouse wheel).</summary>
    NoScrollbar = 1u << 3,
    /// <summary>The mouse wheel does not scroll the region.</summary>
    NoScrollWithMouse = 1u << 4,
    /// <summary>No background.</summary>
    NoBackground = 1u << 7,
    /// <summary>The vertical scrollbar is always shown.</summary>
    AlwaysVerticalScrollbar = 1u << 14,
}

/// <summary>Flags of the sized <see cref="UiDraw.Selectable(string, bool, float, float, UiSelectableFlags)"/>.</summary>
[Flags]
public enum UiSelectableFlags : uint
{
    /// <summary>No flags.</summary>
    None = 0,
    /// <summary>The row spans every column of a table.</summary>
    SpanAllColumns = 1u << 1,
    /// <summary>A double click counts as a click too.</summary>
    AllowDoubleClick = 1u << 2,
    /// <summary>The row is greyed out and takes no clicks.</summary>
    Disabled = 1u << 3,
    /// <summary>Widgets drawn over it later take their own clicks.</summary>
    AllowOverlap = 1u << 4,
    /// <summary>Drawn as if hovered.</summary>
    Highlight = 1u << 5,
}

/// <summary>Where <see cref="UiDraw.DrawRect(UiLayer, float, float, float, float, Color, float, float)"/> draws.</summary>
public enum UiLayer : uint
{
    /// <summary>The current window: clipped to it, scrolls with it, under the widgets drawn after.</summary>
    Window = 0,
    /// <summary>Behind every window.</summary>
    Background = 1,
    /// <summary>Over everything.</summary>
    Foreground = 2,
}

/// <summary>A colour of the current look (<see cref="UiDraw.PushColor"/>).</summary>
public enum UiColorSlot : uint
{
    /// <summary>Text.</summary>
    Text,
    /// <summary>Disabled (greyed) text.</summary>
    TextDisabled,
    /// <summary>A window's background.</summary>
    WindowBg,
    /// <summary>A child region's background.</summary>
    ChildBg,
    /// <summary>Borders.</summary>
    Border,
    /// <summary>The background of a framed widget (a check box, a slider, a text field).</summary>
    FrameBg,
    /// <summary>The frame's background while hovered.</summary>
    FrameBgHovered,
    /// <summary>The frame's background while held.</summary>
    FrameBgActive,
    /// <summary>A button.</summary>
    Button,
    /// <summary>A button while hovered.</summary>
    ButtonHovered,
    /// <summary>A button while held.</summary>
    ButtonActive,
    /// <summary>Selectables: selected.</summary>
    Header,
    /// <summary>Selectables: hovered.</summary>
    HeaderHovered,
    /// <summary>Selectables: held.</summary>
    HeaderActive,
    /// <summary>Separator lines.</summary>
    Separator,
    /// <summary>A scrollbar's background.</summary>
    ScrollbarBg,
    /// <summary>A scrollbar's handle.</summary>
    ScrollbarGrab,
    /// <summary>The gamepad / keyboard focus outline.</summary>
    NavCursor,
    /// <summary>The tick of a check box.</summary>
    CheckMark,
    /// <summary>A slider's handle.</summary>
    SliderGrab,
    /// <summary>A title bar (since 0.2.0-dev.29.16, as are the slots below; an older Kiln ignores them).</summary>
    TitleBg,
    /// <summary>The focused window's title bar.</summary>
    TitleBgActive,
    /// <summary>Drop-downs and tooltips.</summary>
    PopupBg,
    /// <summary>A tab.</summary>
    Tab,
    /// <summary>A tab while hovered.</summary>
    TabHovered,
    /// <summary>The selected tab.</summary>
    TabSelected,
    /// <summary>The header row of a table.</summary>
    TableHeaderBg,
    /// <summary>Selected text in a text field.</summary>
    TextSelectedBg,
}

/// <summary>A spacing or rounding of the current look (<see cref="UiDraw.PushVar"/>); pixels (multiply by
/// <see cref="UiDraw.Scale"/>), the <c>Align</c> ones 0..1.</summary>
public enum UiStyleVar : uint
{
    /// <summary>The padding inside a window (x, y).</summary>
    WindowPadding,
    /// <summary>The padding inside a framed widget such as a button or a text field (x, y).</summary>
    FramePadding,
    /// <summary>The space between widgets (x, y).</summary>
    ItemSpacing,
    /// <summary>The space between the parts of one widget, such as a check box and its label (x, y).</summary>
    ItemInnerSpacing,
    /// <summary>The corner rounding of framed widgets.</summary>
    FrameRounding,
    /// <summary>The border width of framed widgets.</summary>
    FrameBorderSize,
    /// <summary>The corner rounding of child regions.</summary>
    ChildRounding,
    /// <summary>The border width of child regions.</summary>
    ChildBorderSize,
    /// <summary>Where a selectable's text sits inside it, 0 to 1 along each axis (x, y).</summary>
    SelectableTextAlign,
    /// <summary>Where a button's label sits inside it, 0 to 1 along each axis (x, y).</summary>
    ButtonTextAlign,
    /// <summary>The width of a scrollbar.</summary>
    ScrollbarSize,
    /// <summary>The border width of windows.</summary>
    WindowBorderSize,
}

/// <summary>Keys for <see cref="UiDraw.KeyPressed"/> (keyboard and gamepad).</summary>
public enum UiKey : uint
{
    /// <summary>The Tab key.</summary>
    Tab = 1,
    /// <summary>The left arrow.</summary>
    Left,
    /// <summary>The right arrow.</summary>
    Right,
    /// <summary>The up arrow.</summary>
    Up,
    /// <summary>The down arrow.</summary>
    Down,
    /// <summary>Page Up.</summary>
    PageUp,
    /// <summary>Page Down.</summary>
    PageDown,
    /// <summary>Home.</summary>
    Home,
    /// <summary>End.</summary>
    End,
    /// <summary>Enter.</summary>
    Enter,
    /// <summary>Escape.</summary>
    Escape,
    /// <summary>Backspace.</summary>
    Backspace,
    /// <summary>The letter F (Ctrl+F: find).</summary>
    F,
    /// <summary>The gamepad's left shoulder button.</summary>
    GamepadLB,
    /// <summary>The gamepad's right shoulder button.</summary>
    GamepadRB,
    /// <summary>The left face button (Xbox X).</summary>
    GamepadX,
    /// <summary>The top face button (Xbox Y).</summary>
    GamepadY,
    /// <summary>The gamepad's Start button.</summary>
    GamepadStart,
    /// <summary>The gamepad's Back button.</summary>
    GamepadBack,
}

/// <summary>Where the next widget goes (<see cref="UiDraw.Cursor"/>). X/Y are window-local (for
/// <see cref="UiDraw.SetCursor"/>), ScreenX/ScreenY screen pixels (for the Draw calls).</summary>
/// <param name="X">Where the next widget goes, x, window-local (scrolling included).</param>
/// <param name="Y">Where the next widget goes, y, window-local (scrolling included).</param>
/// <param name="ScreenX">The same position's x in screen pixels.</param>
/// <param name="ScreenY">The same position's y in screen pixels.</param>
/// <param name="AvailWidth">The width left in the current window or child region, from the cursor.</param>
/// <param name="AvailHeight">The height left in the current window or child region, from the cursor.</param>
/// <param name="WindowX">The current window's (or child region's) left edge in screen pixels.</param>
/// <param name="WindowY">The current window's (or child region's) top edge in screen pixels.</param>
/// <param name="WindowWidth">The current window's (or child region's) width in pixels.</param>
/// <param name="WindowHeight">The current window's (or child region's) height in pixels.</param>
/// <param name="FontSize">The current font size in pixels, the UI scale applied.</param>
/// <param name="LineHeight">The height of one text line plus the item spacing, in pixels.</param>
/// <param name="ScrollY">The vertical scroll position.</param>
/// <param name="ScrollMaxY">The largest vertical scroll position.</param>
public readonly record struct UiCursor(float X, float Y, float ScreenX, float ScreenY, float AvailWidth, float AvailHeight,
    float WindowX, float WindowY, float WindowWidth, float WindowHeight, float FontSize, float LineHeight, float ScrollY,
    float ScrollMaxY);

/// <summary>The widget drawn last (<see cref="UiDraw.LastItem"/>), in screen pixels.</summary>
/// <param name="MinX">The widget's left edge.</param>
/// <param name="MinY">The widget's top edge.</param>
/// <param name="MaxX">The widget's right edge.</param>
/// <param name="MaxY">The widget's bottom edge.</param>
/// <param name="Hovered">The mouse is over it.</param>
/// <param name="Active">It is held: the mouse is down on it, or a text field is being edited.</param>
/// <param name="Focused">It has the gamepad or keyboard navigation focus.</param>
/// <param name="Clicked">The left button went down over it this frame.</param>
/// <param name="Visible">It is not clipped away.</param>
/// <param name="Edited">Its value changed this frame.</param>
/// <param name="Activated">It became active this frame.</param>
/// <param name="Deactivated">It stopped being active this frame.</param>
public readonly record struct UiItem(float MinX, float MinY, float MaxX, float MaxY, bool Hovered, bool Active, bool Focused,
    bool Clicked, bool Visible, bool Edited, bool Activated, bool Deactivated)
{
    /// <summary>The widget's width in pixels.</summary>
    public float Width => MaxX - MinX;
    /// <summary>The widget's height in pixels.</summary>
    public float Height => MaxY - MinY;
}

public sealed unsafe partial class UiDraw
{
    private static UiNative.Table3* T3 => UiNative.T3;

    /// <summary>This kiln.ui has the layout calls in this file (version 3). Without them each falls back or does nothing.</summary>
    public bool HasLayout => T3 != null;

    /// <summary>Text drawn inside the scope uses <paramref name="size"/> pixels at UI scale 1 (the scale applies on top):
    /// <c>using (ui.FontSize(36)) ui.Text("Mods");</c>. The font stays (the game's under <see cref="UiStyle.Game"/>).</summary>
    public FontScope FontSize(float size)
    {
        var t = T3;
        if (t == null) return new FontScope(false);
        t->PushFontSize(size);
        return new FontScope(true);
    }

    /// <summary>The scope <see cref="FontSize"/> returns; dispose it to go back to the earlier size.</summary>
    /// <param name="pushed">Whether a size was pushed (false on an older kiln.ui, where nothing changes).</param>
    public readonly struct FontScope(bool pushed) : IDisposable
    {
        /// <summary>Ends the font size scope.</summary>
        public void Dispose()
        {
            if (pushed) UiNative.T3->PopFontSize();
        }
    }

    /// <summary>A child region (call <see cref="EndChild"/> always). Width/height 0 = the rest, negative = the rest minus
    /// that much. No outline unless <see cref="UiChildFlags.Border"/>.</summary>
    public bool BeginChild(string id, float width, float height, UiChildFlags flags, UiWindowFlags windowFlags = UiWindowFlags.None)
    {
        var t = T3;
        if (t == null) return T->BeginChild(Utf8.Of(id), width, height, (uint)windowFlags) != 0;
        return t->BeginChildEx(Utf8.Of(id), width, height, (uint)flags, (uint)windowFlags) != 0;
    }

    /// <summary>The width of the next widget (pixels; negative = the rest of the line minus that much).</summary>
    public void NextItemWidth(float width)
    {
        var t = T3;
        if (t != null) t->SetNextItemWidth(width);
    }

    /// <summary>Where the next widget goes and the space left.</summary>
    public UiCursor Cursor
    {
        get
        {
            var t = T3;
            if (t == null) return default;
            UiNative.Cursor c = default;
            c.Size = (uint)sizeof(UiNative.Cursor);
            t->GetCursor(&c);
            return new UiCursor(c.X, c.Y, c.ScreenX, c.ScreenY, c.AvailWidth, c.AvailHeight, c.WindowX, c.WindowY, c.WindowWidth,
                c.WindowHeight, c.FontSize, c.LineHeight, c.ScrollY, c.ScrollMaxY);
        }
    }

    /// <summary>Moves the next widget to (x, y), window-local like <see cref="UiCursor.X"/>.</summary>
    public void SetCursor(float x, float y)
    {
        var t = T3;
        if (t != null) t->SetCursorPos(x, y);
    }

    /// <summary>The rectangle and state of the widget drawn last.</summary>
    public UiItem LastItem
    {
        get
        {
            var t = T3;
            if (t == null) return default;
            UiNative.Item i = default;
            i.Size = (uint)sizeof(UiNative.Item);
            t->GetItem(&i);
            return new UiItem(i.MinX, i.MinY, i.MaxX, i.MaxY, i.Hovered != 0, i.Active != 0, i.Focused != 0, i.Clicked != 0,
                i.Visible != 0, i.Edited != 0, i.Activated != 0, i.Deactivated != 0);
        }
    }

    /// <summary>A selectable row of a size (0 = the rest of the line / one text line). True when clicked or activated
    /// (Enter, gamepad A).</summary>
    public bool Selectable(string label, bool selected, float width, float height = 0, UiSelectableFlags flags = UiSelectableFlags.None)
    {
        var t = T3;
        if (t == null) return Selectable(label, selected);
        return t->SelectableEx(Utf8.Of(label), selected ? (byte)1 : (byte)0, (uint)flags, width, height) != 0;
    }

    /// <summary>The keyboard goes to the next widget (a text field: type into it at once).</summary>
    public void FocusKeyboardHere(int offset = 0)
    {
        var t = T3;
        if (t != null) t->SetKeyboardFocusHere(offset);
    }

    /// <summary>The widget drawn last gets the gamepad / keyboard focus when its window appears.</summary>
    public void DefaultFocus()
    {
        var t = T3;
        if (t != null) t->SetItemDefaultFocus();
    }

    /// <summary>A rectangle on <paramref name="layer"/> (screen pixels). Thickness 0 fills it, else its outline.</summary>
    public void DrawRect(UiLayer layer, float x0, float y0, float x1, float y1, Color color, float rounding = 0, float thickness = 0)
    {
        var t = T3;
        if (t == null)
        {
            DrawRect(x0, y0, x1, y1, color, rounding, thickness <= 0);
            return;
        }
        t->DrawRectIn((uint)layer, x0, y0, x1, y1, color.Rgba, rounding, thickness);
    }

    /// <summary>Text on <paramref name="layer"/> at (x, y) screen pixels; size 0 = the current font size; wrap &gt; 0
    /// wraps at that width. No shadow.</summary>
    public void DrawText(UiLayer layer, float x, float y, string text, Color color, float size = 0, float wrap = 0)
    {
        var t = T3;
        if (t == null)
        {
            DrawText(x, y, text, color, size);
            return;
        }
        t->DrawTextIn((uint)layer, x, y, color.Rgba, size, wrap, Utf8.Of(text));
    }

    /// <summary>The size of <paramref name="text"/> in the current font (size 0 = current size), wrapped at
    /// <paramref name="wrap"/> when &gt; 0.</summary>
    public (float Width, float Height) MeasureText(string text, float size, float wrap)
    {
        var t = T3;
        if (t == null) return MeasureText(text, size);
        float w, h;
        t->TextSizeEx(Utf8.Of(text), size, wrap, &w, &h);
        return (w, h);
    }

    /// <summary>A colour of the current look until the scope ends: <c>using (ui.PushColor(UiColorSlot.Text, gold)) …</c>.</summary>
    public PopScope PushColor(UiColorSlot slot, Color color)
    {
        var t = T3;
        if (t == null) return new PopScope(0);
        t->PushColor((uint)slot, color.Rgba);
        return new PopScope(1);
    }

    /// <summary>A spacing or rounding of the current look until the scope ends (y is ignored for one-value vars).</summary>
    public PopScope PushVar(UiStyleVar var, float x, float y = 0)
    {
        var t = T3;
        if (t == null) return new PopScope(0);
        t->PushVar((uint)var, x, y);
        return new PopScope(2);
    }

    /// <summary>Ends a <see cref="PushColor"/> (kind 1) or <see cref="PushVar"/> (kind 2).</summary>
    public readonly struct PopScope(int kind) : IDisposable
    {
        /// <summary>Takes the colour or spacing off again.</summary>
        public void Dispose()
        {
            if (kind == 1) UiNative.T3->PopColor(1);
            else if (kind == 2) UiNative.T3->PopVar(1);
        }
    }

    /// <summary><paramref name="key"/> went down this frame (only while Kiln has the input). <paramref name="claim"/> takes it
    /// from ImGui's own navigation (Tab then stops moving the focus): claim it every frame you use it.</summary>
    public bool KeyPressed(UiKey key, bool repeat = false, bool claim = false)
    {
        var t = T3;
        return t != null && t->KeyPressed((uint)key, (repeat ? 1u : 0u) | (claim ? 2u : 0u)) != 0;
    }

    /// <summary>The modifier keys held now: Ctrl, Shift and Alt as Dear ImGui's io has them this frame (<c>KeyCtrl</c>,
    /// <c>KeyShift</c>, <c>KeyAlt</c>), from the game window's key messages whether or not Kiln has the input; a focus loss
    /// (alt-tab) clears them, so a release lost to it never leaves one held. For a Shift+drag or Ctrl+wheel inside your
    /// window, no <c>KeyInput</c> listener is needed. <see cref="KeyMods.None"/> outside a frame, and on a kiln.ui older than v3.</summary>
    public KeyMods HeldMods
    {
        get
        {
            var t = T3;
            return t == null ? KeyMods.None : (KeyMods)(t->KeyMods() & 7);
        }
    }

    /// <summary>Scrolls the current window so the cursor sits at <paramref name="ratio"/> (0 top, 0.5 middle, 1 bottom).</summary>
    public void ScrollHere(float ratio = 0.5f)
    {
        var t = T3;
        if (t != null) t->SetScrollHereY(ratio);
    }
}

/// <summary>A font for <see cref="UiDraw.Font"/> (kiln.ui v4).</summary>
public enum UiFont : uint
{
    /// <summary>The current look's font.</summary>
    Default = 0,
    /// <summary>The game's UI font (regular).</summary>
    Game = 1,
    /// <summary>The game's UI font, bold: what its page titles use.</summary>
    GameBold = 2,
}

public sealed unsafe partial class UiDraw
{
    /// <summary>Text drawn inside the scope uses <paramref name="font"/> at the current size:
    /// <c>using (ui.Font(UiFont.GameBold)) ui.Text("MODS");</c>. <see cref="FontChoice.Pushed"/> is false (and nothing changes)
    /// when that font is not loaded or kiln.ui is older than v4.</summary>
    public FontChoice Font(UiFont font)
    {
        var t = UiNative.T4;
        return new FontChoice(t != null && t->PushFont((uint)font) != 0);
    }

    /// <summary>The scope <see cref="Font"/> returns; dispose it to go back to the earlier font.</summary>
    /// <param name="pushed">Whether the font was pushed.</param>
    public readonly struct FontChoice(bool pushed) : IDisposable
    {
        /// <summary>True when the font was pushed; false when it is not loaded or kiln.ui is older than v4.</summary>
        public bool Pushed => pushed;

        /// <summary>Ends the font scope, if it was pushed.</summary>
        public void Dispose()
        {
            if (pushed) UiNative.T4->PopFont();
        }
    }
}
