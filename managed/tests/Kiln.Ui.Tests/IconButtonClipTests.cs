// SPDX-License-Identifier: MIT
// GitHub issue #138 (2), 0.2.0-dev.31: an item icon's ImageButton / ImageButtonTrimmed scrolled out of its window keeps the
// place it takes when shown. Shown, the button is kiln.ui's image_button (ImGui's ImageButton: the image's box plus the frame
// padding) or image_ex's trimmed button; clipped, it was a Dummy of the image's box alone, so a grid's rows jumped as they
// scrolled in. Clipped it is now the same native call with the placeholder (ImGui clips it: nothing drawn), the icon is not
// read or asked for. The rectangle the native calls take is kiln.ui's own host test (style_host_test.cpp images()).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln;

[Collection(FakeUi.UiSerial.Name)]
public sealed unsafe class IconButtonClipTests : IDisposable
{
    private record struct Call(string Kind, string Id, ulong Image, uint Flags, float Width, float Height);

    private static readonly List<Call> Calls = [];
    private static float _cursorY;
    private static UiNative.TableImageEx* _tail;
    private readonly IKilnBackend _previous;
    private readonly List<(Guid, int)> _decoded = [];
    private ulong _next = 100;

    public IconButtonClipTests()
    {
        _previous = Backend.Current;
        FakeUi.FakeUiTable.Install();
        Backend.Current = new FakeUi.FakeUiTable.Backend();
        UiNative.ResetForTests();
        ref var t = ref FakeUi.FakeUiTable.Table->Base.Base.Base;
        t.ImageButton = &ImageButton;
        t.Button = &Button;
        t.Dummy = &Dummy;
        FakeUi.FakeUiTable.Table->Base.GetCursor = &Cursor;
        Calls.Clear();
        _cursorY = 0;
        ItemIcons.ResetForTests();
        ItemIcons.Synchronous = true;
        ItemIcons.Session = () => 1;
        ItemIcons.GameItem = id => new Guid((int)id.Value, 0, 0, new byte[8]);
        ItemIcons.Defined = _ => null;
        ItemIcons.Decode = (g, bucket) =>
        {
            _decoded.Add((g, bucket));
            return (new byte[bucket * bucket * 4], bucket, bucket);
        };
        ItemIcons.Upload = (_, _, _) => ++_next;
        ItemIcons.Free = _ => { };
        ItemIcons.ImageState = _ => UiNative.ImageReady;
        _tail = (UiNative.TableImageEx*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.TableImageEx));
        _tail->ImageEx = &ImageEx;
        UiNative.ImageExOverride = _tail;
    }

    public void Dispose()
    {
        ItemIcons.ResetForTests();
        Backend.Current = _previous;
        UiNative.ResetForTests();
        UiNative.ImageExOverride = null;
        FakeUi.FakeUiTable.Free();
        NativeMemory.Free(_tail);
        _tail = null;
    }

    // a 400 x 300 window at (0, 0); the cursor at _cursorY (screen pixels)
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Cursor(UiNative.Cursor* c)
    {
        var size = c->Size;
        *c = default;
        c->Size = size;
        c->ScreenY = _cursorY;
        c->Y = _cursorY;
        c->WindowWidth = 400;
        c->WindowHeight = 300;
        c->AvailWidth = 400;
        c->AvailHeight = 300;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte ImageButton(NativeStr id, ulong image, float w, float h)
    {
        Calls.Add(new Call("image_button", id.ToString(), image, 0, w, h));
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte Button(NativeStr id, float w, float h)
    {
        Calls.Add(new Call("button", id.ToString(), 0, 0, w, h));
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Dummy(float w, float h) => Calls.Add(new Call("dummy", "", 0, 0, w, h));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte ImageEx(NativeStr id, ulong image, UiNative.ImageDraw* d)
    {
        Calls.Add(new Call("image_ex", id.ToString(), image, d->Flags, d->Width, d->Height));
        return 0;
    }

    private static UiDraw NewUi(float dpi) => new() { Frame = new UiNative.Frame { DpiScale = dpi } };

    private List<Call> Grid(UiDraw ui, bool trim, float scrollY, int rows, int columns, float cell, float width)
    {
        Calls.Clear();
        for (var r = 0; r < rows; r++)
        {
            _cursorY = r * cell - scrollY;
            for (var c = 0; c < columns; c++)
            {
                var id = (uint)(1 + r * columns + c);
                var img = ItemIcons.View(new ItemId(id))!;
                if (trim) ui.ImageButtonTrimmed($"cell{id}", img, width, width);
                else ui.ImageButton($"cell{id}", img, width, width);
            }
        }
        return [.. Calls];
    }

    [Theory]
    [InlineData(false, 1f)]
    [InlineData(true, 1f)]
    [InlineData(false, 1.5f)]
    [InlineData(true, 2f)]
    public void A_clipped_icon_button_takes_the_same_native_call_as_the_shown_one(bool trim, float dpi)
    {
        var ui = NewUi(dpi);
        const float size = 0;  // the image's own size times the UI scale (64 x dpi), as a grid of Ui.ItemImage buttons uses it
        // 10 rows of 6, 140 px apart, in a 300 px window: rows 0..2 show, rows 3..9 are below it
        Grid(ui, trim, 0, rows: 10, columns: 6, cell: 140, width: size);
        // the shown cells were asked for, the clipped ones not: no read, no request for an icon scrolled out
        Assert.Equal(18, _decoded.Count);
        Assert.Equal(18, ItemIcons.Stats.Count);
        var first = Grid(ui, trim, 0, rows: 10, columns: 6, cell: 140, width: size);  // the shown ones' icons are loaded now
        Assert.Equal(60, first.Count);
        Assert.DoesNotContain(first, c => c.Kind is "dummy" or "button");

        // scrolled down by 5 rows: rows 0..4 clip, rows 5..7 show; every cell keeps its call and its size
        var second = Grid(ui, trim, 5 * 140, rows: 10, columns: 6, cell: 140, width: size);
        Assert.Equal(60, second.Count);
        for (var i = 0; i < 60; i++)
            Assert.Equal((first[i].Kind, first[i].Id, first[i].Flags, first[i].Width, first[i].Height),
                (second[i].Kind, second[i].Id, second[i].Flags, second[i].Width, second[i].Height));
        var kind = trim ? "image_ex" : "image_button";
        Assert.All(first.Concat(second), c => Assert.Equal((kind, 64 * dpi, 64 * dpi), (c.Kind, c.Width, c.Height)));
        if (trim) Assert.All(first.Concat(second), c => Assert.Equal(UiNative.ImageDrawTrim | UiNative.ImageDrawFit, c.Flags));
        // the clipped ones drew the placeholder (ImGui clips it), never an icon of theirs; the shown ones their icons
        var placeholder = ItemIcons.Placeholder();
        Assert.All(first.Skip(18), c => Assert.Equal(placeholder, c.Image));
        Assert.All(first.Take(18), c => Assert.NotEqual(placeholder, c.Image));
        Assert.All(second.Take(30), c => Assert.Equal(placeholder, c.Image));
        Assert.Equal(36, ItemIcons.Stats.Count);  // rows 5..7 asked for now, rows 8 and 9 never
    }

    [Fact]
    public void Without_image_ex_the_trimmed_button_falls_back_alike_shown_and_clipped()
    {
        UiNative.ImageExOverride = null;
        var ui = NewUi(1);
        var calls = Grid(ui, trim: true, 0, rows: 10, columns: 2, cell: 140, width: 40);
        Assert.All(calls, c => Assert.Equal(("image_button", 40f, 40f), (c.Kind, c.Width, c.Height)));
    }

    [Fact]
    public void Without_any_overlay_image_shown_and_clipped_cells_are_the_same_plain_button()
    {
        ItemIcons.Upload = (_, _, _) => 0;  // the placeholder cannot be made either
        var ui = NewUi(1);
        foreach (var trim in new[] { false, true })
        {
            var calls = Grid(ui, trim, 0, rows: 10, columns: 2, cell: 140, width: 40);
            Assert.All(calls, c => Assert.Equal(("button", 40f, 40f), (c.Kind, c.Width, c.Height)));
            Assert.All(calls, c => Assert.EndsWith("##icon", c.Id));
        }
    }

    [Fact]
    public void A_mods_own_image_button_is_not_affected()
    {
        var ui = NewUi(1);
        _cursorY = 5000;  // far below the window: a mod's own picture is drawn as always (ImGui clips it)
        ui.ImageButton("mine", new UiImage { Handle = 9, Width = 32, Height = 32 }, 32, 32);
        Assert.Equal(new Call("image_button", "mine", 9, 0, 32, 32), Assert.Single(Calls));
    }
}
