// SPDX-License-Identifier: MIT
// UiDraw's pointer, held mouse buttons and drag and drop between widgets (since 0.2.0-dev.30.3, GitHub issue 113). Native:
// kiln.ui's pointer, drag_source and drop_target (native/core/abi/services/kiln_ui.h, draw_slots_pointer.inl), read from
// Dear ImGui's io. Guide: docs/kiln/ui.md "The pointer, held buttons and drag and drop".
using System.Collections.Concurrent;
using System.Numerics;
using System.Text;

namespace Kiln;

public sealed unsafe partial class UiDraw
{
    /// <summary>
    /// The pointer (the mouse cursor) now, in the overlay's pixels: the space <see cref="DrawRect(float, float, float, float, Color, float, bool)"/>,
    /// <see cref="DrawText(float, float, string, Color?, float)"/>,
    /// <see cref="Width"/> x <see cref="Height"/> and Kiln windows' positions use, with (0, 0) at the top left of the game's
    /// picture. The same space as <see cref="SlotClick.Pointer"/>. Null when the overlay has not seen the cursor yet, or on a
    /// kiln.ui without this query (an older Kiln). Since 0.2.0-dev.30.3 (mod API 0.2.13).
    /// </summary>
    /// <remarks>Kiln reads it from Dear ImGui's io each frame, whether or not Kiln has the input. In the play view the game
    /// recentres the cursor for mouse look, so it means something only while a pointer is shown: a Kiln screen, the Kiln menu,
    /// or one of the game's own menus. Whole pixels.</remarks>
    /// <example><c>if (ui.Pointer is { } p &amp;&amp; ui.ButtonHeld(UiMouseButton.Left)) ui.DrawRect(p.X - 4, p.Y - 4, p.X + 4, p.Y + 4, Color.White);</c></example>
    public Vector2? Pointer => ReadPointer(out var p) && (p.Flags & UiNative.PointerValid) != 0 ? new Vector2(p.X, p.Y) : null;

    /// <summary>
    /// <paramref name="button"/> is held down now (pressed in an earlier frame or this one, not yet released), wherever the
    /// pointer is. With <see cref="Pointer"/> it is enough to build a drag of your own; <see cref="ItemClicked"/> answers a
    /// press on a widget instead. False when the button is up, and on a kiln.ui without this query (an older Kiln). Since
    /// 0.2.0-dev.30.3 (mod API 0.2.13).
    /// </summary>
    /// <remarks>What Dear ImGui's io holds this frame: the window's button messages Kiln saw, also while Kiln does not have
    /// the input (the game gets those presses too).</remarks>
    public bool ButtonHeld(UiMouseButton button = UiMouseButton.Left) =>
        (uint)button <= 2 && ReadPointer(out var p) && (p.Held & (1u << (int)button)) != 0;

    private static bool ReadPointer(out UiNative.Pointer p)
    {
        p = default;
        var t = UiNative.Drag;
        if (t == null || t->PointerFn == null) return false;
        p.Size = (uint)sizeof(UiNative.Pointer);
        fixed (UiNative.Pointer* q = &p) return t->PointerFn(q) != 0;
    }

    /// <summary>
    /// Makes the widget drawn last a drag source (Dear ImGui's drag and drop): press it and move, and while the button is
    /// held it carries <paramref name="payload"/> as a payload of <paramref name="kind"/>, with <paramref name="preview"/>
    /// drawn by the pointer. Call it right after the widget, every frame. True on the frames it is being dragged. A
    /// <see cref="DropTarget"/> of the same kind takes the payload when it is released there. Works after any widget, also
    /// one without an id (text, an image). Since 0.2.0-dev.30.3 (mod API 0.2.13).
    /// </summary>
    /// <param name="kind">What the payload is (<c>"stack"</c>, <c>"chest-slot"</c>): your own name, matched exactly. Kinds are
    /// your mod's own: another mod's drop target of the same name never gets your payload, nor you theirs.</param>
    /// <param name="payload">The text carried, at most 4096 bytes of UTF-8 (an id, a slot index, some JSON).</param>
    /// <param name="preview">Text drawn next to the pointer while it is dragged; null or empty: none.</param>
    /// <returns>True while this widget is being dragged; false otherwise, and on a kiln.ui without drag and drop (an older Kiln).</returns>
    /// <exception cref="ArgumentException"><paramref name="kind"/> is null or empty, or <paramref name="payload"/> is longer than 4096 bytes.</exception>
    /// <example><code>
    /// ui.Selectable(stack.Name);
    /// ui.DragSource("stack", stack.Slot.ToString(), stack.Name);
    /// </code></example>
    public bool DragSource(string kind, string payload, string? preview = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(kind);
        ArgumentNullException.ThrowIfNull(payload);
        if (Encoding.UTF8.GetByteCount(payload) > UiNative.DragPayloadMax)
            throw new ArgumentException($"a drag payload holds at most {UiNative.DragPayloadMax} bytes of UTF-8", nameof(payload));
        var t = UiNative.Drag;
        if (t == null || t->DragSource == null) return false;
        var bytes = Utf8.Of(payload);
        return t->DragSource(Utf8.Of(UiDragKinds.Of(UiModCalls.Caller, kind)), bytes.Ptr, bytes.Len, Utf8.Of(preview ?? "")) != 0;
    }

    [ThreadStatic] private static byte[]? _dropBuf;

    /// <summary>
    /// Makes the widget drawn last a drop target for payloads of <paramref name="kind"/> (Dear ImGui's drag and drop): the
    /// payload's text on the frame a <see cref="DragSource"/> of that kind (of your mod) is released over it, else null.
    /// Call it right after the widget, every frame. While such a payload hovers the widget, it is highlighted. Since
    /// 0.2.0-dev.30.3 (mod API 0.2.13).
    /// </summary>
    /// <returns>The payload once, on the release; null on every other frame, and on a kiln.ui without drag and drop (an older Kiln).</returns>
    /// <exception cref="ArgumentException"><paramref name="kind"/> is null or empty.</exception>
    /// <example><code>
    /// ui.Selectable("Chest");
    /// if (ui.DropTarget("stack") is { } slot) Move(int.Parse(slot), toChest: true);
    /// </code></example>
    public string? DropTarget(string kind)
    {
        ArgumentException.ThrowIfNullOrEmpty(kind);
        var t = UiNative.Drag;
        if (t == null || t->DropTarget == null) return null;
        _dropBuf ??= GC.AllocateArray<byte>(UiNative.DragPayloadMax, pinned: true);
        nuint len = 0;
        bool got;
        fixed (byte* b = _dropBuf) got = t->DropTarget(Utf8.Of(UiDragKinds.Of(UiModCalls.Caller, kind)), b, (nuint)_dropBuf.Length, &len) != 0;
        return got ? Encoding.UTF8.GetString(_dropBuf, 0, (int)Math.Min(len, (nuint)_dropBuf.Length)) : null;
    }
}

/// <summary>The kind kiln.ui gets for a mod's drag kind: "k" and the FNV-1a 64 of "&lt;mod id&gt;\n&lt;kind&gt;" in hex (17 bytes:
/// within ImGui's 32, never ImGui's own "_..."), so each mod's kinds are its own and any kind fits.</summary>
internal static class UiDragKinds
{
    private static readonly ConcurrentDictionary<(string Mod, string Kind), string> Cache = new();

    public static string Of(ModContext? mod, string kind) => Of(mod?.Info.Id ?? "", kind);

    internal static string Of(string mod, string kind)
    {
        if (Cache.TryGetValue((mod, kind), out var s)) return s;
        var h = 0xcbf29ce484222325UL;
        foreach (var b in Encoding.UTF8.GetBytes(mod + "\n" + kind))
        {
            h ^= b;
            h *= 0x100000001b3UL;
        }
        s = "k" + h.ToString("x16");
        if (Cache.Count < 4096) Cache.TryAdd((mod, kind), s);  // kinds are a mod's fixed names; a mod making them up per frame is not cached
        return s;
    }
}
