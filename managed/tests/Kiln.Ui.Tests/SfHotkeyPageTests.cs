// SPDX-License-Identifier: MIT
// A ShroudForge mod's shortcut setting on the Mods page (ShroudForgeUi), drawn through the real page code against a fake
// kiln.ui table (netjunk, 2026-11-26: Altar Cleanup's key field refused "Ü" twice and took only "SDL:47"). No game.
using System.Text.Json.Nodes;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.SfHotkeyPageTests;

[Collection(UiSerial.Name)]
public sealed class SfHotkeyPageTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private readonly Func<uint, uint, uint>? _layout = SfShortcuts.MapVirtualKey;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-sfhotkey-" + Guid.NewGuid().ToString("N"));
    private readonly UiDraw _ui = new();

    public SfHotkeyPageTests()
    {
        _previous = Backend.Current;
        FakeUiTable.Install();
        Backend.Current = new FakeUiTable.Backend();
        UiNative.ResetForTests();
        ShroudForgeUi.ResetForTests();
        SfShortcuts.MapVirtualKey = null;
        _ui.Frame.DpiScale = 1;
        _ui.Frame.Width = 1920;
        _ui.Frame.Height = 1080;
    }

    public void Dispose()
    {
        UiEngine.EndMenuFrame();
        UiEngine.EndMenuFrame();
        ShroudForgeUi.ResetForTests();
        SfShortcuts.MapVirtualKey = _layout;
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
        try { Directory.Delete(_root, true); }
        catch (IOException) { }
    }

    private string Package(string value)
    {
        var folder = Path.Combine(_root, "mods", "altar-cleanup");
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "mod.json"), """{"id":"altar-cleanup","name":"Altar Cleanup","version":"1.0.0","capabilities":["runtime"]}""");
        File.WriteAllText(Path.Combine(folder, "extended.mod.json"),
            "{\n  \"schemaVersion\": 1,\n  \"enabled\": true,\n  \"settings\": { \"cleanup\": { \"value\": \"" + value +
            "\", \"control\": \"keybind\", \"label\": \"Clean up altar zone\", \"applyAt\": \"live\" } }\n}\n");
        ShroudForgeUi.Load(folder);
        return folder;
    }

    private static ModManagerPage Page(string folder) => new()
    {
        Source = () => new CatalogSnapshot
        {
            Version = 1,
            Mods = [new CatalogMod { Engine = "shroudforge", Id = "altar-cleanup", Name = "Altar Cleanup", Folder = folder, State = ModRunState.Loaded }],
            Engines = [new CatalogEngine { Kind = "shroudforge", Name = "ShroudForge", Count = 1 }],
        },
        Filter = new EngineFilter("shroudforge"), Selected = "shroudforge/altar-cleanup",
    };

    private void Frame(ModManagerPage page)
    {
        _ui.Frame.Number++;
        lock (FakeUiTable.Texts) FakeUiTable.Texts.Clear();
        page.Draw(_ui, new PageRect(0, 0, 1800, 900), 1);
        UiEngine.EndMenuFrame();
        FakeUiTable.Pressed.Clear();
    }

    private static string Value(string folder) =>
        JsonNode.Parse(File.ReadAllText(Path.Combine(folder, "extended.mod.json")))!["settings"]!["cleanup"]!["value"]!.GetValue<string>();

    private static bool Shown(string text)
    {
        lock (FakeUiTable.Texts) return FakeUiTable.Texts.Any(t => t.Contains(text, StringComparison.Ordinal));
    }

    // the player typed into the shortcut's text field and left it
    private void Type(ModManagerPage page, string text)
    {
        FakeUiTable.Item = new UiNative.Item { Active = 1, Visible = 1 };
        FakeUiTable.Type = _ => text;
        Frame(page);
        FakeUiTable.Type = null;
        FakeUiTable.Item = new UiNative.Item { Deactivated = 1, Visible = 1 };
        Frame(page);
        FakeUiTable.Item = default;
        ShroudForgeUi.Flush();
    }

    [Theory]
    [InlineData("Ctrl+G", "Ctrl+SDL:10")]
    [InlineData("F5", "SDL:62")]
    [InlineData("LeftBracket", "SDL:47")]
    [InlineData("SDL:47", "SDL:47")]
    [InlineData("Shift+Mouse4", "Shift+Mouse:4")]
    public void A_shortcut_typed_the_way_Kiln_names_keys_is_saved_in_ShroudForges_form(string typed, string saved)
    {
        var folder = Package("SDL:66");
        var page = Page(folder);
        Frame(page);
        Type(page, typed);
        Assert.Equal(saved, Value(folder));
    }

    [Fact]
    public void A_key_typed_as_its_character_on_a_German_keyboard_is_that_key()
    {
        SfShortcuts.MapVirtualKey = (code, type) => (code, type) switch
        {
            (0x1A, 3) => 0xBA, (0xBA, 4) => 0x1A, (0xBA, 2) => 'Ü', _ => 0,
        };
        var folder = Package("SDL:66");
        var page = Page(folder);
        Frame(page);
        Type(page, "Ü");
        Assert.Equal("SDL:47", Value(folder));
        ShroudForgeUi.Load(folder);
        Frame(page);
        Frame(page);
        Assert.True(Shown("Ü"), string.Join(" | ", FakeUiTable.Texts));
    }

    [Fact]
    public void Text_that_is_no_shortcut_is_refused_in_plain_words_and_the_file_keeps_its_value()
    {
        var folder = Package("SDL:66");
        var page = Page(folder);
        Frame(page);
        Type(page, "cleanup key");
        Assert.Equal("SDL:66", Value(folder));
        Frame(page);
        Frame(page);
        Assert.True(Shown("Clean up altar zone: Kiln cannot read \"cleanup key\" as a key."), string.Join(" | ", FakeUiTable.Texts));
    }

    [Fact]
    public void The_shortcut_is_shown_by_Kilns_key_names()
    {
        var folder = Package("Ctrl+SDL:10|Mouse:4");
        var page = Page(folder);
        Frame(page);
        Frame(page);
        Assert.True(Shown("Ctrl + G / Mouse4"), string.Join(" | ", FakeUiTable.Texts));
    }
}
