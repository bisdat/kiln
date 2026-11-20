// SPDX-License-Identifier: MIT
// Key names and physical keys (GitHub issue 33, 0.2.0-dev.28). A key kiln.ui has no name for was saved as "Key223" and
// lost at the next start (KeyChord.TryParse did not read it); every key a chord can hold now prints a name that parses
// back. The punctuation names (Backquote, Minus, Quote, ...) name the key at that place of a US keyboard: on a UK, German
// or French layout the key left of 1 sends another virtual key (VK_OEM_8, VK_OEM_5, VK_OEM_7), and a binding on
// "Backquote" follows its scan code (0x29 on every layout). kiln.ui's half (the names it draws, the menu key) is
// native/services/ui/key_names.h and tests/key_names_test.cpp; the last test here holds the two tables together.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.KeyNameTests;

[Collection(UiSerial.Name)]
public sealed class KeyNameTests : IDisposable
{
    private const BindingFlags Static = BindingFlags.NonPublic | BindingFlags.Static;
    private static readonly FieldInfo Remapping = typeof(UiEngine).GetField("_remapping", Static)!;
    private readonly IKilnBackend _previous = Backend.Current;
    private readonly string _previousRoot = KilnPaths.Root;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-keynames-" + Guid.NewGuid().ToString("N"));
    private readonly List<KeyBinding> _bindings = [];

    public KeyNameTests()
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

    private static UiNative.Key Key(uint vk, uint scan, bool down = true) =>
        new() { Size = 20, Vk = vk, Scan = scan, Down = down ? (byte)1 : (byte)0 };

    private static bool Press(uint vk, uint scan)
    {
        var consumed = UiEngine.Key(Key(vk, scan));
        UiEngine.Key(Key(vk, scan, down: false));
        return consumed;
    }

    private string File => Path.Combine(_root, "config", "keybinds.json");

    private static readonly KeyMods[] EveryMods =
        [.. Enumerable.Range(0, 8).Select(i => (KeyMods)i)];

    // ---- names ----

    [Fact]
    public void Every_key_a_chord_can_hold_prints_a_name_that_parses_back()
    {
        // 1 and 2 are the left and right mouse buttons: never bound (Kiln's screens need them)
        var codes = Enumerable.Range(3, 253).Select(i => (uint)i).Concat([0x100u, 0x101u, 0x102u, 0x103u]);
        var failed = new List<string>();
        foreach (var vk in codes)
            foreach (var mods in EveryMods)
            {
                var chord = new KeyChord(vk, mods);
                var text = chord.ToString();
                if (!KeyChord.TryParse(text, out var back) || back != chord)
                    failed.Add($"0x{vk:X2} {mods}: \"{text}\" -> {(KeyChord.TryParse(text, out back) ? $"0x{back.VirtualKey:X2} {back.Mods}" : "unparsed")}");
            }
        Assert.True(failed.Count == 0, string.Join("\n", failed));
    }

    [Theory]
    [InlineData("Key223", 0xDFu, KeyMods.None)]  // what dev.27 saved for VK_OEM_8 (issue 33)
    [InlineData("Ctrl+Key223", 0xDFu, KeyMods.Ctrl)]
    [InlineData("Key7", 0x07u, KeyMods.None)]
    [InlineData("Key255", 0xFFu, KeyMods.None)]
    [InlineData("key65", 0x41u, KeyMods.None)]
    [InlineData("Oem8", 0xDFu, KeyMods.None)]
    [InlineData("IntlBackslash", 0xE2u, KeyMods.None)]
    [InlineData("IntlRo", 0xC1u, KeyMods.None)]
    [InlineData("OemClear", 0xFEu, KeyMods.None)]
    [InlineData("Shift+Alt+Oem8", 0xDFu, KeyMods.Shift | KeyMods.Alt)]
    [InlineData("Ctrl", 0x11u, KeyMods.None)]  // a modifier key by itself: the last word is the key
    [InlineData("Shift+Ctrl", 0x11u, KeyMods.Shift)]
    [InlineData("Ctrl+Ctrl", 0x11u, KeyMods.Ctrl)]
    [InlineData("LeftShift", 0xA0u, KeyMods.None)]
    [InlineData("PrintScreen", 0x2Cu, KeyMods.None)]
    public void Key_numbers_and_the_new_names_parse(string text, uint vk, KeyMods mods)
    {
        Assert.True(KeyChord.TryParse(text, out var c), text);
        Assert.Equal(new KeyChord(vk, mods), c);
    }

    [Theory]
    [InlineData(0xDFu, "Oem8")]
    [InlineData(0xE2u, "IntlBackslash")]
    [InlineData(0xC0u, "Backquote")]
    [InlineData(0x07u, "Key7")]
    [InlineData(0x2Du, "Insert")]  // the names no longer depend on which kiln-ui.dll answers
    [InlineData(0x11u, "Ctrl")]
    public void Keys_print_their_names(uint vk, string name) => Assert.Equal(name, new KeyChord(vk, KeyMods.None).ToString());

    [Theory]
    [InlineData("Key0")]
    [InlineData("Key256")]
    [InlineData("Key-1")]
    [InlineData("Key 9")]
    [InlineData("Key")]
    [InlineData("Ctrl+")]
    [InlineData("Ctrl+Shift+")]
    [InlineData("Key223+Key65")]
    public void Bad_key_numbers_are_refused(string text) => Assert.False(KeyChord.TryParse(text, out _), text);

    [Theory]
    [InlineData("Key1")]
    [InlineData("Ctrl+Key2")]
    public void The_left_and_right_buttons_stay_unbindable_by_number(string text)
    {
        Assert.False(KeyChord.TryParse(text, out _));
        Assert.Contains("cannot be bound", Assert.Throws<FormatException>(() => KeyChord.Parse(text)).Message);
    }

    // ---- keybinds.json ----

    [Fact]
    public void A_key_captured_as_Key223_survives_a_restart()
    {
        // as 0.2.0-dev.27 wrote it after the Keys page took a UK keyboard's backquote key (issue 33)
        System.IO.File.WriteAllText(File, """
            {
              "comment": "Kiln key bindings: id -> key (F8, Ctrl+K, Shift+Mouse4, Ctrl+WheelUp, None). Edited by the Kiln menu (Keys).",
              "bindings": {
                "t.noclip": "Key223",
                "t.chord": "Ctrl+Key223",
                "t.quote": "Backquote",
                "t.mouse": "Shift+Mouse4",
                "t.none": "None"
              }
            }
            """);
        Assert.Equal(new KeyChord(0xDF, KeyMods.None), Register("t.noclip", "Backquote").Key);
        Assert.Equal(new KeyChord(0xDF, KeyMods.Ctrl), Register("t.chord", "F8").Key);
        Assert.Equal(new KeyChord(0xC0, KeyMods.None), Register("t.quote", "F8").Key);
        Assert.Equal(new KeyChord(0x05, KeyMods.Shift), Register("t.mouse", "F8").Key);
        Assert.True(Register("t.none", "F8").Key.IsNone);
        // the dev.27 binding still fires on the key it was captured from
        var n = 0;
        _bindings[0].OnPress = () => n++;
        Assert.True(Press(0xDF, 0x29));
        Assert.Equal(1, n);
    }

    // ---- physical keys ----

    [Theory]
    [InlineData(0xC0u, 0x29u)]  // US: VK_OEM_3
    [InlineData(0xDFu, 0x29u)]  // UK: VK_OEM_8 (issue 33)
    [InlineData(0xDCu, 0x29u)]  // German: VK_OEM_5 (^)
    [InlineData(0xDEu, 0x29u)]  // French: VK_OEM_7 (²)
    [InlineData(0xC0u, 0x00u)]  // no scan code (an injected key): the US virtual key
    public void Backquote_is_the_key_left_of_1_on_every_layout(uint vk, uint scan)
    {
        var n = 0;
        Register("t.console", "Backquote", () => n++);
        Assert.True(Press(vk, scan));
        Assert.Equal(1, n);
    }

    [Fact]
    public void Backquote_does_not_fire_on_the_key_that_sends_its_US_code_elsewhere()
    {
        var console = 0;
        var quote = 0;
        Register("t.console", "Backquote", () => console++);
        Register("t.quote", "Quote", () => quote++);
        // UK: the ' key (scan 0x28, right of ;) sends VK_OEM_3, the US backquote's code
        Assert.True(Press(0xC0, 0x28));
        Assert.Equal(0, console);
        Assert.Equal(1, quote);
        // UK: the ` key (scan 0x29) sends VK_OEM_8
        Assert.True(Press(0xDF, 0x29));
        Assert.Equal(1, console);
        Assert.Equal(1, quote);
    }

    [Fact]
    public void Letters_stay_virtual_keys_where_a_layout_moves_them()
    {
        var s = 0;
        var semicolon = 0;
        Register("t.s", "S", () => s++);
        Register("t.semicolon", "Semicolon", () => semicolon++);
        Assert.True(Press(0x53, 0x27));  // Dvorak: S sits where US has ;
        Assert.Equal(1, s);
        Assert.Equal(0, semicolon);
    }

    [Fact]
    public void The_Keys_page_saves_the_physical_name_of_a_punctuation_key()
    {
        var b = Register("t.capture", "F8");
        Remapping.SetValue(null, b.Id);
        FakeUiTable.MenuOpen = true;  // the Kiln menu on Keys
        try
        {
            UiEngine.Key(Key(0xDF, 0x29));  // UK backquote key
        }
        finally
        {
            FakeUiTable.MenuOpen = false;
        }
        Assert.Equal(new KeyChord(0xC0, KeyMods.None), b.Key);
        var saved = (JsonObject)JsonNode.Parse(System.IO.File.ReadAllText(File))!["bindings"]!;
        Assert.Equal("Backquote", (string?)saved["t.capture"]);
        // and it fires on that key after a restart
        ForgetSaved();
        b.Dispose();
        _bindings.Remove(b);
        var n = 0;
        Register("t.capture", "F8", () => n++);
        Assert.True(Press(0xDF, 0x29));
        Assert.Equal(1, n);
    }

    // ---- kiln.ui's table ----

    private static string Header([CallerFilePath] string here = "") =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(here)!, "..", "..", "..", "native", "services", "ui", "key_names.h"));

    private static string Block(string text, string name)
    {
        var start = text.IndexOf($"{name}[] = {{", StringComparison.Ordinal);
        Assert.True(start >= 0, $"key_names.h has no {name}");
        var end = text.IndexOf("};", start, StringComparison.Ordinal);
        return text[start..end];
    }

    [Fact]
    public void Kiln_Ui_and_kiln_ui_name_keys_and_places_alike()
    {
        var text = System.IO.File.ReadAllText(Header());
        var names = Regex.Matches(Block(text, "kNames"), @"\{0x([0-9A-F]+), ""([A-Za-z0-9]+)""\}")
            .Select(m => (Convert.ToUInt32(m.Groups[1].Value, 16), m.Groups[2].Value)).ToArray();
        var places = Regex.Matches(Block(text, "kPlaces"), @"\{0x([0-9A-F]+), 0x([0-9A-F]+)\}")
            .Select(m => (Convert.ToUInt32(m.Groups[1].Value, 16), Convert.ToUInt32(m.Groups[2].Value, 16))).ToArray();
        Assert.True(names.Length > 100, $"read {names.Length} names from key_names.h");
        Assert.Equal(names, KeyNames.Table);
        Assert.Equal(places, KeyNames.Places);
        // the rule for an OEM code, as text (key_names.h oem()), against KeyNames.Oem for every code
        var oem = Regex.Match(text, @"constexpr bool oem\(uint32_t vk\) \{\s*return (.*?);", RegexOptions.Singleline).Groups[1].Value;
        var parts = Regex.Matches(oem, @"\(vk >= 0x([0-9A-F]+) && vk <= 0x([0-9A-F]+)\)|vk == 0x([0-9A-F]+)");
        Assert.NotEmpty(parts);
        bool Native(uint vk) => parts.Any(m => m.Groups[3].Success
            ? vk == Convert.ToUInt32(m.Groups[3].Value, 16)
            : vk >= Convert.ToUInt32(m.Groups[1].Value, 16) && vk <= Convert.ToUInt32(m.Groups[2].Value, 16));
        for (uint vk = 0; vk <= 0x103; vk++) Assert.True(Native(vk) == KeyNames.Oem(vk), $"oem(0x{vk:X2}) differs");
    }
}
