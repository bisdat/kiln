// SPDX-License-Identifier: MIT
// GitHub issue 117: Ui.Inventory's marks are drawn on Kiln's overlay, above the game's UI, so a mark under the game's item
// menu (RMB on a slot: Equip, Salvage, Delete) sat on top of the menu. kiln.ui's popups (kiln_ui.h, 0.2.0-dev.31.9) gives
// the game's popup panels of the last UI frame; DrawMarks leaves out the marks of the slots they cover, all of them while a
// panel's rectangle is unknown or the error popup shows, and draws as before when Kiln does not read popups.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace UiInventoryTestSuite;

[Collection(UiSerial.Name)]
public sealed unsafe class MarksPopupTests : IDisposable
{
    private static UiNative.Popups _answer;
    private static int _rc;

    public MarksPopupTests()
    {
        UiNative.PopupsOverride = null;
        _answer = default;
        _rc = 0;
    }

    public void Dispose() => UiNative.PopupsOverride = null;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakePopups(UiNative.Popups* out_)
    {
        if (out_->Size < (uint)sizeof(UiNative.Popups)) return 1;
        *out_ = _answer;
        out_->Size = (uint)sizeof(UiNative.Popups);
        return _rc;
    }

    // a 4 x 2 grid of 80-unit slots at (100, 100), a mark on every one
    private static List<VisibleSlot> Grid()
    {
        var list = new List<VisibleSlot>();
        for (var row = 0; row < 2; row++)
            for (var col = 0; col < 4; col++)
            {
                var s = new UiValuesNative.VisibleSlot
                {
                    Size = (uint)sizeof(UiValuesNative.VisibleSlot), Available = UiValuesNative.OuterRect, Item = 0xAA,
                    Count = 1, SlotIndex = (byte)(row * 4 + col),
                    OuterUi = new UiValuesNative.Rect { X = 100 + col * 80, Y = 100 + row * 80, Width = 80, Height = 80 },
                };
                list.Add(UiInventoryBridge.ToVisible(s));
            }
        return list;
    }

    private static readonly SlotMark Gold = new(Color.Yellow);

    private static List<int> Marked(UiNative.Popups? popups, float scale = 2)
    {
        var m = new UiInventoryNative.Mapping { Ok = 1, ScaleX = scale, ScaleY = scale };
        return UiInventoryBridge.MarksToDraw(Grid(), m, [], [(_ => Gold, null)], popups)
            .Select(d => (int)d.Slot.SlotIndex).ToList();
    }

    private static UiNative.Popups Popups(uint flags, params (float X, float Y, float W, float H)[] rects)
    {
        var p = new UiNative.Popups { Size = (uint)sizeof(UiNative.Popups), Available = 1, Flags = flags, Count = (uint)rects.Length, Frame = 9 };
        var r = rects.Select(x => new UiNative.PopupRect { X = x.X, Y = x.Y, Width = x.W, Height = x.H }).ToArray();
        if (r.Length > 0) p.R0 = r[0];
        if (r.Length > 1) p.R1 = r[1];
        if (r.Length > 2) p.R2 = r[2];
        if (r.Length > 3) p.R3 = r[3];
        return p;
    }

    [Fact]
    public void Without_the_games_popups_every_mark_is_drawn_as_before()
    {
        Assert.Equal([0, 1, 2, 3, 4, 5, 6, 7], Marked(null));
        Assert.Equal([0, 1, 2, 3, 4, 5, 6, 7], Marked(Popups(0)));
    }

    [Fact]
    public void The_item_menu_hides_the_marks_of_the_slots_it_covers_and_only_those()
    {
        // the menu opened from slot 1 at its middle, 330 x 180 units: over slots 1-3 of row 0 and 5-7 of row 1
        var menu = Popups(UiNative.PopupPanel, (220, 140, 330, 180));
        Assert.Equal([0, 4], Marked(menu));
        // the reporter's case: the menu's top-left corner over one slot; a slot that only touches an edge keeps its mark
        Assert.Equal([0, 1, 2, 3, 4, 5, 6], Marked(Popups(UiNative.PopupPanel, (390, 230, 330, 180))));
        Assert.Equal([0, 1, 2, 3, 4, 5, 6, 7], Marked(Popups(UiNative.PopupPanel, (420, 100, 330, 180))));
        // the drawn boxes of the kept marks stay where the mapping puts them
        var m = new UiInventoryNative.Mapping { Ok = 1, ScaleX = 2, ScaleY = 2, OffsetX = 5, OffsetY = 7 };
        var kept = UiInventoryBridge.MarksToDraw(Grid(), m, [], [(_ => Gold, null)], menu);
        Assert.Equal((205f, 207f, 365f, 367f), (kept[0].X0, kept[0].Y0, kept[0].X1, kept[0].Y1));
    }

    [Fact]
    public void Each_of_several_panels_hides_what_it_covers()
    {
        var two = Popups(UiNative.PopupPanel, (90, 90, 20, 20), (410, 250, 10, 10));
        Assert.Equal([1, 2, 3, 4, 5, 6], Marked(two));
    }

    [Fact]
    public void An_unknown_panel_rectangle_or_the_error_popup_hides_every_mark()
    {
        Assert.Empty(Marked(Popups(UiNative.PopupPanel | UiNative.PopupUnknown)));
        Assert.Empty(Marked(Popups(UiNative.PopupPanel | UiNative.PopupUnknown, (0, 0, 1, 1))));
        Assert.Empty(Marked(Popups(UiNative.PopupError)));
    }

    [Fact]
    public void A_count_past_the_native_maximum_reads_only_the_four_rectangles()
    {
        var p = Popups(UiNative.PopupPanel, (100, 100, 1, 1));
        p.Count = 99;  // never past R3
        Assert.Equal([1, 2, 3, 4, 5, 6, 7], Marked(p));
    }

    [Fact]
    public void ReadPopups_is_null_without_the_slot_when_unavailable_and_the_state_otherwise()
    {
        UiNative.ResetForTests();
        Assert.Null(UiNative.ReadPopups());  // no kiln.ui table, no override: marks as before
        UiNative.PopupsOverride = &FakePopups;
        _answer = Popups(UiNative.PopupPanel, (1, 2, 3, 4));
        var got = UiNative.ReadPopups();
        Assert.NotNull(got);
        Assert.Equal(UiNative.PopupPanel, got!.Value.Flags);
        Assert.Equal(3f, got.Value.R0.Width);
        _answer.Available = 0;
        Assert.Null(UiNative.ReadPopups());
        _answer.Available = 1;
        _rc = 3;  // KILN_ERR_UNAVAILABLE
        Assert.Null(UiNative.ReadPopups());
    }

    [Fact]
    public void The_struct_matches_the_native_layout()
    {
        Assert.Equal(88, sizeof(UiNative.Popups));
        Assert.Equal(16, (int)Marshal.OffsetOf<UiNative.Popups>(nameof(UiNative.Popups.Frame)));
        Assert.Equal(24, (int)Marshal.OffsetOf<UiNative.Popups>(nameof(UiNative.Popups.R0)));
        Assert.Equal(1088, sizeof(UiNative.TablePopups));
    }
}
