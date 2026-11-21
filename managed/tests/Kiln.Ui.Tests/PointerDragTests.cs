// SPDX-License-Identifier: MIT
// UiDraw.Pointer, ButtonHeld, DragSource and DropTarget (0.2.0-dev.30.3, GitHub issue 113) against a fake of kiln.ui's
// pointer tail: what each slot gets (the mod-scoped kind, the payload's bytes, the preview) and what the managed side makes
// of the answers, an older kiln.ui without the tail, the argument rules, and the tail's offsets. ImGui's own behaviour (a
// drag from one widget dropped on another, delivered once on release) is the host test's:
// native/services/ui/tests/pointer_drag_test.cpp.
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace PointerDragTestSuite;

[Collection(UiSerial.Name)]
public sealed unsafe class PointerDragTests : IDisposable
{
    private static UiNative.TableDrag* _table;
    private static UiNative.Pointer _pointer;
    private static byte _pointerOk;
    private static byte _dragAnswer;
    private static string? _drop;
    private static nuint _capacity;
    private static readonly List<(string Kind, string Payload, string Preview)> Sources = [];
    private static readonly List<string> Targets = [];

    public PointerDragTests()
    {
        _table = (UiNative.TableDrag*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.TableDrag));
        _table->PointerFn = &Pointer;
        _table->DragSource = &DragSource;
        _table->DropTarget = &DropTarget;
        UiNative.DragOverride = _table;
        _pointer = default;
        _pointerOk = 1;
        _dragAnswer = 0;
        _drop = null;
        Sources.Clear();
        Targets.Clear();
    }

    public void Dispose()
    {
        UiNative.DragOverride = null;
        NativeMemory.Free(_table);
        _table = null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte Pointer(UiNative.Pointer* p)
    {
        if (p->Size < (uint)sizeof(UiNative.Pointer)) return 0;
        if (_pointerOk == 0) return 0;
        var size = p->Size;
        *p = _pointer;
        p->Size = size;
        return 1;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte DragSource(NativeStr kind, byte* payload, nuint len, NativeStr preview)
    {
        Sources.Add((kind.ToString(), Encoding.UTF8.GetString(payload, (int)len), preview.ToString()));
        return _dragAnswer;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte DropTarget(NativeStr kind, byte* buf, nuint capacity, nuint* len)
    {
        Targets.Add(kind.ToString());
        _capacity = capacity;
        if (_drop is null) return 0;
        var bytes = Encoding.UTF8.GetBytes(_drop);
        var n = Math.Min(bytes.Length, (int)capacity);
        bytes.AsSpan(0, n).CopyTo(new Span<byte>(buf, n));
        *len = (nuint)bytes.Length;
        return 1;
    }

    private static string Fnv(string s)
    {
        var h = 0xcbf29ce484222325UL;
        foreach (var b in Encoding.UTF8.GetBytes(s))
        {
            h ^= b;
            h *= 0x100000001b3UL;
        }
        return "k" + h.ToString("x16");
    }

    [Fact]
    public void The_tail_follows_chat_search()
    {
        Assert.Equal(1056, (int)Marshal.OffsetOf<UiNative.TableDrag>(nameof(UiNative.TableDrag.PointerFn)));
        Assert.Equal(1064, (int)Marshal.OffsetOf<UiNative.TableDrag>(nameof(UiNative.TableDrag.DragSource)));
        Assert.Equal(1072, (int)Marshal.OffsetOf<UiNative.TableDrag>(nameof(UiNative.TableDrag.DropTarget)));
        Assert.Equal(1080, sizeof(UiNative.TableDrag));
        Assert.Equal(24, sizeof(UiNative.Pointer));
        Assert.Equal(8, (int)Marshal.OffsetOf<UiNative.Pointer>(nameof(UiNative.Pointer.X)));
        Assert.Equal(16, (int)Marshal.OffsetOf<UiNative.Pointer>(nameof(UiNative.Pointer.Held)));
    }

    [Fact]
    public void Pointer_is_the_overlays_position_only_when_imgui_has_one()
    {
        var ui = new UiDraw();
        Assert.Null(ui.Pointer);  // not seen yet (no VALID flag)
        _pointer = new UiNative.Pointer { Flags = UiNative.PointerValid, X = 812, Y = 377 };
        Assert.Equal(new Vector2(812, 377), ui.Pointer);
        _pointerOk = 0;  // outside a frame: the slot says no
        Assert.Null(ui.Pointer);
    }

    [Fact]
    public void ButtonHeld_reads_the_held_bits()
    {
        var ui = new UiDraw();
        _pointer = new UiNative.Pointer { Held = 1u | 4u };  // left and middle; no position needed
        Assert.True(ui.ButtonHeld());
        Assert.True(ui.ButtonHeld(UiMouseButton.Left));
        Assert.False(ui.ButtonHeld(UiMouseButton.Right));
        Assert.True(ui.ButtonHeld(UiMouseButton.Middle));
        Assert.False(ui.ButtonHeld((UiMouseButton)3));
        Assert.False(ui.ButtonHeld((UiMouseButton)(-1)));
        _pointerOk = 0;
        Assert.False(ui.ButtonHeld(UiMouseButton.Left));
    }

    [Fact]
    public void DragSource_passes_a_mod_scoped_kind_the_payload_and_the_preview()
    {
        var ui = new UiDraw();
        Assert.False(ui.DragSource("stack", "slot:3", "Iron Bar"));
        _dragAnswer = 1;
        Assert.True(ui.DragSource("stack", "slot:4"));
        Assert.Equal(2, Sources.Count);
        // Kiln's own code here (no mod): "k" + FNV-1a 64 of "\nstack"; 17 bytes, within ImGui's 32
        Assert.Equal((Fnv("\nstack"), "slot:3", "Iron Bar"), Sources[0]);
        Assert.Equal((Fnv("\nstack"), "slot:4", ""), Sources[1]);
        Assert.Equal(17, Sources[0].Kind.Length);
        // UTF-8 payloads up to 4096 bytes; an empty one is a payload too
        Assert.True(ui.DragSource("stack", "Zweihänder ⚔"));
        Assert.Equal("Zweihänder ⚔", Sources[^1].Payload);
        Assert.True(ui.DragSource("stack", ""));
        Assert.True(ui.DragSource("stack", new string('x', 4096)));
    }

    [Fact]
    public void DropTarget_gives_the_payload_once_delivered_and_null_otherwise()
    {
        var ui = new UiDraw();
        Assert.Null(ui.DropTarget("stack"));
        Assert.Equal(Fnv("\nstack"), Assert.Single(Targets));
        Assert.Equal((nuint)4096, _capacity);  // KILN_UI_DRAG_PAYLOAD_MAX: no payload is ever cut
        _drop = "slot:3";
        Assert.Equal("slot:3", ui.DropTarget("stack"));
        _drop = "ünïcödé";
        Assert.Equal("ünïcödé", ui.DropTarget("stack"));
        _drop = "";
        Assert.Equal("", ui.DropTarget("stack"));
        _drop = new string('y', 4096);
        Assert.Equal(4096, ui.DropTarget("stack")!.Length);
    }

    [Fact]
    public void Kinds_are_each_mods_own()
    {
        Assert.Equal(UiDragKinds.Of("mod-a", "stack"), UiDragKinds.Of("mod-a", "stack"));
        Assert.NotEqual(UiDragKinds.Of("mod-a", "stack"), UiDragKinds.Of("mod-b", "stack"));
        Assert.NotEqual(UiDragKinds.Of("mod-a", "stack"), UiDragKinds.Of("mod-a", "slot"));
        Assert.Equal(Fnv("mod-a\nstack"), UiDragKinds.Of("mod-a", "stack"));
        // any length or text of kind fits ImGui's 32 bytes and is never ImGui's own "_..."
        var long_ = UiDragKinds.Of("mod-a", "_" + new string('k', 500));
        Assert.Equal(17, long_.Length);
        Assert.StartsWith("k", long_);
    }

    [Fact]
    public void Bad_arguments_throw()
    {
        var ui = new UiDraw();
        Assert.Throws<ArgumentException>(() => ui.DragSource("", "x"));
        Assert.Throws<ArgumentNullException>(() => ui.DragSource(null!, "x"));
        Assert.Throws<ArgumentNullException>(() => ui.DragSource("k", null!));
        Assert.Throws<ArgumentException>(() => ui.DragSource("k", new string('x', 4097)));
        Assert.Throws<ArgumentException>(() => ui.DragSource("k", new string('ä', 2049)));  // 4098 bytes of UTF-8
        Assert.Throws<ArgumentException>(() => ui.DropTarget(""));
        Assert.Throws<ArgumentNullException>(() => ui.DropTarget(null!));
        Assert.Empty(Sources);
        Assert.Empty(Targets);
    }

    [Fact]
    public void An_older_kiln_ui_answers_nothing()
    {
        UiNative.DragOverride = null;
        var previous = Backend.Current;
        var v4 = (UiNative.Table4*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.Table4));
        v4->Base.Base.Base.Header = new ServiceHeader { Size = (uint)sizeof(UiNative.Table4), Version = 4 };
        try
        {
            // a kiln.ui whose table ends before the tail (0.2.0-dev.30.2 and older)
            FakeUiTable.Table = v4;
            Backend.Current = new FakeUiTable.Backend();
            UiNative.ResetForTests();
            Assert.True(UiNative.T != null);
            var ui = new UiDraw();
            Assert.Null(ui.Pointer);
            Assert.False(ui.ButtonHeld());
            Assert.False(ui.DragSource("stack", "x"));
            Assert.Null(ui.DropTarget("stack"));
        }
        finally
        {
            FakeUiTable.Table = null;
            Backend.Current = previous;
            UiNative.ResetForTests();
            NativeMemory.Free(v4);
        }
        _table->PointerFn = null;  // a tail with a null slot
        UiNative.DragOverride = _table;
        Assert.Null(new UiDraw().Pointer);
        Assert.Empty(Sources);
    }
}
