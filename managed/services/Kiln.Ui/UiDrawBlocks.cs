// SPDX-License-Identifier: MIT
// UiDraw's calls over kiln.ui.blocks (kiln_ui_blocks.h, 0.2.0-dev.31): a tab selected from code (BeginControlledTab), a
// tree node's open state set from code, the edit state of the widget drawn last and a text field with Kiln's own flags
// (TextField). New names, not overloads: BeginTab and InputText keep their dev.26 shapes (Kiln.Contract.Tests). The
// richer UI blocks (Kiln.Blocks) draw with them; mods may too. On a Kiln without the table they fall back to the plain calls.
using System.Text;

namespace Kiln;

/// <summary>What the widget drawn last is doing (<see cref="UiDraw.LastItemState"/>).</summary>
[Flags]
public enum UiItemState : uint
{
    /// <summary>Nothing: not active, nothing happened this frame.</summary>
    None = 0,
    /// <summary>It is being used: a text field has the keyboard, a button is held.</summary>
    Active = 1,
    /// <summary>It stopped being active this frame (a text field lost the keyboard: Enter, Escape, Tab, a click elsewhere).</summary>
    Deactivated = 2,
    /// <summary>It stopped being active this frame after its value was edited.</summary>
    DeactivatedAfterEdit = 4,
    /// <summary>Its value was edited this frame.</summary>
    Edited = 8,
}

/// <summary>How <see cref="UiDraw.TextField"/> behaves.</summary>
[Flags]
public enum UiTextFieldFlags : uint
{
    /// <summary>A one-line field that answers true on every change.</summary>
    None = 0,
    /// <summary>Answers true only when Enter is pressed (the text still changes as the player types).</summary>
    EnterReturns = 1,
    /// <summary>Shown and selectable, not editable.</summary>
    ReadOnly = 2,
    /// <summary>Several lines; Enter is a new line there (<see cref="EnterReturns"/> is ignored).</summary>
    Multiline = 4,
}

public sealed unsafe partial class UiDraw
{
    private const uint TabSelect = 1, ItemOpenAlways = 0;
    private const uint KInputEnter = 1, KInputReadOnly = 2, KInputMultiline = 4;

    /// <summary>Whether this Kiln has the calls below (kiln.ui.blocks); without it they fall back to the plain ones and a
    /// tab or tree node cannot be selected or opened from code.</summary>
    public bool HasBlockWidgets => UiNative.Blocks != null;

    /// <summary>A tab of a tab bar (<see cref="BeginTabBar"/>) that code can select: with <paramref name="select"/> it
    /// becomes the selected tab (from this frame or the next); without, the player's pick stays. True while it is the
    /// selected tab: draw its content, then call <see cref="EndTab"/>. On a Kiln without kiln.ui.blocks it is
    /// <see cref="BeginTab"/>.</summary>
    public bool BeginControlledTab(string label, bool select)
    {
        var b = UiNative.Blocks;
        if (b == null) return BeginTab(label);
        return b->BeginTabItemEx(Utf8.Of(label), select ? TabSelect : 0) != 0;
    }

    /// <summary>Opens or closes the next <see cref="TreeNode"/> this frame, whatever the player did before.</summary>
    public void SetNextItemOpen(bool open)
    {
        var b = UiNative.Blocks;
        if (b != null) b->SetNextItemOpen(open ? (byte)1 : (byte)0, ItemOpenAlways);
    }

    /// <summary>The edit state of the widget drawn last; <see cref="UiItemState.None"/> on a Kiln without kiln.ui.blocks.</summary>
    public UiItemState LastItemState
    {
        get
        {
            var b = UiNative.Blocks;
            return b == null ? UiItemState.None : (UiItemState)b->ItemState();
        }
    }

    /// <summary>
    /// A text field editing <paramref name="value"/> (at most <paramref name="maxBytes"/> UTF-8 bytes, 2..65536), with
    /// <paramref name="flags"/> and a size in pixels (0: the default). Returns true when the text changed (with
    /// <see cref="UiTextFieldFlags.EnterReturns"/>: when Enter was pressed). A value that does not fit or holds a NUL is
    /// shown cut and read-only, and <paramref name="value"/> stays as it is. Use <see cref="LastItemState"/> right after it to
    /// see whether the field lost the keyboard.
    /// </summary>
    public bool TextField(string label, ref string value, int maxBytes, UiTextFieldFlags flags = UiTextFieldFlags.None, float width = 0, float height = 0)
    {
        ArgumentNullException.ThrowIfNull(value);
        var b = UiNative.Blocks;
        if (b == null)
            return (flags & UiTextFieldFlags.ReadOnly) == 0 && InputText(label, ref value, maxBytes);
        maxBytes = Math.Clamp(maxBytes, 2, 1 << 16);
        if (_textBuf == null || _textBuf.Length < maxBytes + 1) _textBuf = GC.AllocateArray<byte>(maxBytes + 1, pinned: true);
        var whole = Fill(value, _textBuf, maxBytes);
        uint f = 0;
        if ((flags & UiTextFieldFlags.EnterReturns) != 0) f |= KInputEnter;
        if ((flags & UiTextFieldFlags.ReadOnly) != 0 || !whole) f |= KInputReadOnly;
        if ((flags & UiTextFieldFlags.Multiline) != 0) f |= KInputMultiline;
        bool changed;
        fixed (byte* p = _textBuf) changed = b->InputTextEx(Utf8.Of(label), p, (nuint)(maxBytes + 1), f, width, height) != 0;
        if (!whole || (f & KInputReadOnly) != 0) return false;
        var len = Array.IndexOf(_textBuf, (byte)0, 0, maxBytes + 1);
        var now = Encoding.UTF8.GetString(_textBuf, 0, len < 0 ? maxBytes : len);
        if (now != value) value = now;
        return changed;
    }
}
