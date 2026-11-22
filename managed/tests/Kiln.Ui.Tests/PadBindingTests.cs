// SPDX-License-Identifier: MIT
// Key bindings on a game controller (GitHub issue 119 (a), since 0.2.0-dev.31): "Pad:LS", "Pad:LB+A". The names in
// KeyChord (parse, print, refusals), keybinds.json, the Kiln menu's key (never a pad button), UiEngine.Key (a pad press
// fires its binding with the pad's modifiers, never while a Kiln screen has the input) and the Keys page's "press a key"
// (a combination, or LB alone on its release). kiln.ui's half (the presses from XInput, what the game keeps seeing) is
// native/services/ui/tests/pad_keys_test.cpp. No game: a fake kiln.ui table.
using System.Reflection;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.PadBindingTests;

[Collection(UiSerial.Name)]
public sealed class PadBindingTests : IDisposable
{
    private const BindingFlags Static = BindingFlags.NonPublic | BindingFlags.Static;
    private static readonly FieldInfo Remapping = typeof(UiEngine).GetField("_remapping", Static)!;
    private readonly IKilnBackend _previous = Backend.Current;
    private readonly string _previousRoot = KilnPaths.Root;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-padbinds-" + Guid.NewGuid().ToString("N"));
    private readonly List<KeyBinding> _bindings = [];

    public PadBindingTests()
    {
        FakeUiTable.Install();
        Backend.Current = new FakeUiTable.Backend();
        UiNative.ResetForTests();
        Directory.CreateDirectory(Path.Combine(_root, "config"));
        KilnPaths.Root = _root;
        KilnPaths.Config = null!;
        ForgetSaved();
    }

    public void Dispose()
    {
        Remapping.SetValue(null, null);
        foreach (var b in _bindings) b.Dispose();
        ForgetSaved();
        KilnPaths.Root = _previousRoot;
        KilnPaths.Config = null!;
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
        try { Directory.Delete(_root, true); }
        catch (IOException) { }
    }

    private static void ForgetSaved()
    {
        typeof(KeyBindings).GetField("_loaded", Static)!.SetValue(null, false);
        typeof(KeyBindings).GetField("_saved", Static)!.SetValue(null, new Dictionary<string, string>(StringComparer.Ordinal));
    }

    private KeyBinding Register(string id, string key, Action? onPress = null)
    {
        var b = KeyBindings.Register(id, key, onPress, id, "Tests");
        _bindings.Add(b);
        return b;
    }

    // kiln_ui.h KILN_UI_VK_PAD_* and KILN_UI_MOD_PAD_*
    private const uint LS = 0x206, LB = 0x208, RB = 0x209, A = 0x20C, B = 0x20D, X = 0x20E, Start = 0x204, LT = 0x210;

    private static bool Send(uint vk, KeyMods mods = KeyMods.None, bool down = true, bool captured = false) =>
        UiEngine.Key(new UiNative.Key { Size = 20, Vk = vk, Mods = (uint)mods, Down = down ? (byte)1 : (byte)0, Captured = captured ? (byte)1 : (byte)0 });

    [Theory]
    [InlineData("Pad:LS", 0x206u, KeyMods.None, "Pad:LS")]
    [InlineData("Pad:LB+A", 0x20Cu, KeyMods.PadLB, "Pad:LB+A")]
    [InlineData("pad:lt+rs", 0x207u, KeyMods.PadLT, "Pad:LT+RS")]
    [InlineData("Pad:RB+LB+Y", 0x20Fu, KeyMods.PadLB | KeyMods.PadRB, "Pad:LB+RB+Y")]
    [InlineData("Pad:LB", 0x208u, KeyMods.None, "Pad:LB")]
    [InlineData("Pad:LT+RT", 0x211u, KeyMods.PadLT, "Pad:LT+RT")]
    [InlineData("Pad:Start", 0x204u, KeyMods.None, "Pad:Start")]
    [InlineData("Pad:Up", 0x200u, KeyMods.None, "Pad:Up")]
    public void Pad_names_parse_and_print_back(string text, uint vk, KeyMods mods, string printed)
    {
        Assert.True(KeyChord.TryParse(text, out var c), text);
        Assert.Equal(new KeyChord(vk, mods), c);
        Assert.True(c.IsPad);
        Assert.Equal(printed, c.ToString());
        Assert.True(KeyChord.TryParse(printed, out var again) && again == c);
    }

    [Theory]
    [InlineData("Ctrl+Pad:A")]
    [InlineData("Pad:A+B")]
    [InlineData("Pad:LB+LB")]
    [InlineData("Pad:")]
    [InlineData("Pad:Q")]
    [InlineData("Pad:LB+")]
    [InlineData("Pad:Shift+A")]
    public void Wrong_pad_names_are_refused_with_a_reason(string text)
    {
        Assert.False(KeyChord.TryParse(text, out _));
        var e = Assert.Throws<FormatException>(() => KeyChord.Parse(text));
        Assert.Contains("Pad:", e.Message);
    }

    [Fact]
    public void A_keyboard_chord_is_not_a_pad_chord()
    {
        Assert.False(KeyChord.Parse("Ctrl+K").IsPad);
        Assert.False(new KeyPress(0x4B, 0x25, KeyMods.Ctrl, true, false, false, false).IsPad);
        Assert.True(new KeyPress(A, 0, KeyMods.PadLB, true, false, false, false).IsPad);
        Assert.Equal("Pad:LB+A", new KeyPress(A, 0, KeyMods.PadLB, true, false, false, false).Chord.ToString());
    }

    [Fact]
    public void A_pad_press_fires_its_binding_with_the_pads_modifiers_and_is_kept()
    {
        var fav = 0;
        var combo = 0;
        var l3 = Register("t.fav", "Pad:LS", () => fav++);
        Register("t.combo", "Pad:LB+A", () => combo++);
        Assert.True(Send(LS));
        Assert.True(l3.IsDown);
        Assert.Equal(1, fav);
        Assert.False(Send(LS, down: false));  // a release keeps nothing
        Assert.False(l3.IsDown);
        Assert.False(Send(A));                 // A alone is not LB+A, and reaches the game
        Assert.True(Send(A, KeyMods.PadLB));
        Assert.Equal(1, combo);
        // a keyboard Ctrl bit never makes a pad chord (kiln.ui sends pad modifiers only), nor the other way round
        Assert.False(Send(A, KeyMods.Ctrl));
        Assert.Equal(1, combo);
    }

    [Fact]
    public void No_pad_binding_fires_while_a_Kiln_screen_has_the_input()
    {
        var n = 0;
        Register("t.fav", "Pad:LS", () => n++);
        Assert.False(Send(LS, captured: true));  // the pad navigates the screen there
        Assert.Equal(0, n);
    }

    [Fact]
    public void The_Keys_page_takes_a_pad_combination_or_a_shoulder_button_alone()
    {
        var b = Register("t.x", "F9");
        Remapping.SetValue(null, "t.x");
        FakeUiTable.MenuOpen = true;
        try
        {
            Assert.True(Send(LB));                     // held: a combination may follow
            Assert.Equal("F9", b.Key.ToString());
            Assert.True(Send(A, KeyMods.PadLB));
            Assert.Equal("Pad:LB+A", b.Key.ToString());
            Assert.Null(Remapping.GetValue(null));
            Assert.False(Send(LB, down: false));      // its release after the combination binds nothing

            Remapping.SetValue(null, "t.x");
            Assert.True(Send(LT));
            Assert.True(Send(LT, down: false));       // let go alone: LT itself
            Assert.Equal("Pad:LT", b.Key.ToString());

            Remapping.SetValue(null, "t.x");
            Assert.True(Send(B));                     // B is a button like any other here (a kept B is not Escape)
            Assert.Equal("Pad:B", b.Key.ToString());
            Assert.Contains("\"t.x\": \"Pad:B\"", File.ReadAllText(Path.Combine(_root, "config", "keybinds.json")));
        }
        finally
        {
            FakeUiTable.MenuOpen = false;
        }
    }

    [Fact]
    public void The_Kiln_menus_key_is_never_a_pad_button_and_saved_pad_names_load()
    {
        Assert.False(KeyBindings.Rebind(KeyBindings.MenuId, "Pad:Start"));
        Assert.Equal(KeyBindings.MenuDefault, KeyBindings.MenuKey);
        File.WriteAllText(Path.Combine(_root, "config", "keybinds.json"), """{"bindings": {"t.y": "Pad:RB+X"}}""");
        ForgetSaved();
        var y = Register("t.y", "F10");
        Assert.Equal(new KeyChord(X, KeyMods.PadRB), y.Key);
        Assert.Equal(Start, KeyChord.Parse("Pad:Start").VirtualKey);
        Assert.Equal(RB, KeyChord.Parse("Pad:RB").VirtualKey);
    }
}
