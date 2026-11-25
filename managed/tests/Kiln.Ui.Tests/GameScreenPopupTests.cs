// SPDX-License-Identifier: MIT
// GameScreen.PopupOpen (0.2.0-dev.31.5, GitHub issue 117): one of the game's popups is open in the UI frame the screen is
// of, from kiln.ui's popups; false for an older screen, without the slot and when Kiln does not read popups.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace UiInventoryTestSuite;

[Collection(UiSerial.Name)]
public sealed unsafe class GameScreenPopupTests : IDisposable
{
    private static UiNative.Popups _answer;

    public GameScreenPopupTests()
    {
        UiNative.PopupsOverride = null;
        _answer = default;
    }

    public void Dispose() => UiNative.PopupsOverride = null;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakePopups(UiNative.Popups* out_)
    {
        *out_ = _answer;
        out_->Size = (uint)sizeof(UiNative.Popups);
        return out_->Available != 0 ? 0 : 3;
    }

    private static UiNative.Popups Open(uint flags, ulong frame) =>
        new() { Size = (uint)sizeof(UiNative.Popups), Available = 1, Flags = flags, Frame = frame };

    private static GameScreen Screen(ulong frame)
    {
        var s = new UiNative.Screen { Size = (uint)sizeof(UiNative.Screen), UiState = 7, HudScreen = 9, Available = 1, Frame = frame };
        return GameScreen.From(s);
    }

    [Fact]
    public void PopupOpenAt_takes_the_screens_frame_or_the_next_and_nothing_older()
    {
        Assert.False(GameScreen.PopupOpenAt(40, null));
        Assert.False(GameScreen.PopupOpenAt(40, Open(0, 40)));
        Assert.True(GameScreen.PopupOpenAt(40, Open(UiNative.PopupPanel, 40)));
        Assert.True(GameScreen.PopupOpenAt(40, Open(UiNative.PopupPanel | UiNative.PopupUnknown, 41)));
        Assert.True(GameScreen.PopupOpenAt(40, Open(UiNative.PopupError, 40)));
        Assert.False(GameScreen.PopupOpenAt(40, Open(UiNative.PopupPanel, 42)));  // a screen read longer ago
        Assert.False(GameScreen.PopupOpenAt(40, Open(UiNative.PopupPanel, 39)));
        Assert.False(GameScreen.PopupOpenAt(0, Open(UiNative.PopupPanel, 0)));    // no frame yet
    }

    [Fact]
    public void A_screen_says_whether_a_popup_is_open_in_its_frame()
    {
        UiNative.ResetForTests();
        Assert.False(Screen(40).PopupOpen);  // no kiln.ui: as before
        UiNative.PopupsOverride = &FakePopups;
        _answer = Open(UiNative.PopupPanel, 40);
        Assert.True(Screen(40).PopupOpen);
        Assert.False(Screen(30).PopupOpen);
        _answer = Open(0, 41);
        Assert.False(Screen(40).PopupOpen);
        _answer = Open(UiNative.PopupPanel, 40);
        _answer.Available = 0;  // ui.popups off on this build
        Assert.False(Screen(40).PopupOpen);
        Assert.False(GameScreen.None.PopupOpen);
    }
}
