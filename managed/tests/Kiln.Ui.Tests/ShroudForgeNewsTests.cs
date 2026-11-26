// SPDX-License-Identifier: MIT
// A ShroudForge mod's news (shroudforge.notifications.publish) in the Kiln menu when Kiln hosts ShroudForge: the
// notices its runtime keeps in state.json "events" (fork pin create_mod_api.rs + write_notification_event.rs) read
// back, drawn in the mod's details, and a mod's toggle_modloader() opening the page on the mod that just published.
// netjunk, 2026-11-26: Altar Cleanup's "Ask before cleaning up" scans, publishes what it found with "press the key
// again within 60 s", and opens ShroudForge's window; Kiln opened its page with none of that on it. No game.
using Kiln.FakeUi;
using Xunit;

namespace Kiln.ShroudForgeNewsTests;

[Collection(UiSerial.Name)]
public sealed class ShroudForgeNewsTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-sfnews-" + Guid.NewGuid().ToString("N"));
    private readonly UiDraw _ui = new();
    private string StatePath => Path.Combine(_root, "shroudforge", "state.json");

    public ShroudForgeNewsTests()
    {
        _previous = Backend.Current;
        FakeUiTable.Install();
        Backend.Current = new FakeUiTable.Backend();
        UiNative.ResetForTests();
        ShroudForgeUi.ResetForTests();
        ShroudForgeNews.ResetForTests();
        ShroudForgeNews.StatePath = () => StatePath;
        Directory.CreateDirectory(Path.GetDirectoryName(StatePath)!);
        _ui.Frame.DpiScale = 1;
        _ui.Frame.Width = 1920;
        _ui.Frame.Height = 1080;
        ModManagerHost.Page.Section = ModManagerSection.Mods;
        ModManagerHost.Page.Filter = EngineFilter.All;
    }

    public void Dispose()
    {
        ModManagerHost.Page.CancelRequest();
        ModManagerHost.Page.Filter = EngineFilter.All;
        ModManagerHost.Page.Update(CatalogSnapshot.Empty with { Version = -2 }, false);
        ShroudForgeNews.ResetForTests();
        ShroudForgeUi.ResetForTests();
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
        try { Directory.Delete(_root, true); }
        catch (IOException) { }
    }

    // What ShroudForge's runtime writes for notifications.publish{id, title, message, level}: the state file's "events"
    // section, keyed "<mod>-<id>.json", the notice's id namespaced "<mod>.<id>", updatedAt in Unix seconds.
    private static string Event(string mod, string id, string title, string message, long at, string level = "info") =>
        $$"""
        "{{mod}}-{{id}}.json": {"id":"{{mod}}.{{id}}","modId":"{{mod}}","title":"{{title}}","message":"{{message}}","level":"{{level}}","actionUrl":null,"updatedAt":{{at}}}
        """;

    private void State(params string[] events) =>
        File.WriteAllText(StatePath, "{\"schemaVersion\":1,\"runtime\":{\"running\":true},\"events\":{" + string.Join(",", events) + "}}");

    [Fact]
    public void Reads_the_notices_ShroudForge_keeps_in_its_state_file_newest_first()
    {
        State(
            Event("altar-cleanup", "undo", "Undo", "Nothing to undo", 50),
            Event("altar-cleanup", "result", "Altar Cleanup: 487 blocks found", "Press Keypad 1 again within 60 seconds to clean up.", 100),
            Event("glider", "hello", "Glider", "Ready", 70, "warn"),
            // not what the runtime writes: left out, the rest still read
            "\"broken-1.json\": {\"id\":\"x.y\",\"modId\":\"x\",\"message\":\"no title\",\"level\":\"info\",\"updatedAt\":1}",
            "\"broken-2.json\": {\"id\":\"x.y\",\"modId\":3,\"title\":\"t\",\"message\":\"m\",\"level\":\"info\",\"updatedAt\":1}",
            "\"broken-3.json\": \"text\"");
        var news = ShroudForgeNews.Read(StatePath);
        Assert.Equal(["altar-cleanup.result", "glider.hello", "altar-cleanup.undo"], news.Select(n => n.Id));
        var top = news[0];
        Assert.Equal(("altar-cleanup", "Altar Cleanup: 487 blocks found", "Press Keypad 1 again within 60 seconds to clean up.", "info", 100L),
            (top.ModId, top.Title, top.Message, top.Level, top.UpdatedAt));
        Assert.Equal(["altar-cleanup.result", "altar-cleanup.undo"], ShroudForgeNews.ForMod(news, "altar-cleanup").Select(n => n.Id));
    }

    [Fact]
    public void A_missing_or_unreadable_state_file_has_no_news()
    {
        Assert.Empty(ShroudForgeNews.Read(StatePath));
        File.WriteAllText(StatePath, "{\"events\":");
        Assert.Empty(ShroudForgeNews.Read(StatePath));
        File.WriteAllText(StatePath, "{\"events\":[]}");
        Assert.Empty(ShroudForgeNews.Read(StatePath));
    }

    [Fact]
    public void The_fresh_mod_is_the_one_whose_notice_came_in_the_last_seconds()
    {
        State(Event("glider", "hello", "Glider", "Ready", 95), Event("altar-cleanup", "result", "Found", "Press again", 100));
        var news = ShroudForgeNews.Read(StatePath);
        Assert.Equal("altar-cleanup", ShroudForgeNews.Fresh(news, nowSeconds: 101, sinceSeconds: 99));
        Assert.Null(ShroudForgeNews.Fresh(news, nowSeconds: 200, sinceSeconds: 198));  // an old notice opens nothing
        Assert.Null(ShroudForgeNews.Fresh(news, nowSeconds: 50, sinceSeconds: 48));  // nor one dated ahead of the clock
    }

    private string Package(string id, string settings)
    {
        var folder = Path.Combine(_root, "mods", id);
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "mod.json"), $$"""{"id":"{{id}}","name":"{{id}}","version":"1.0.0","capabilities":["runtime"]}""");
        File.WriteAllText(Path.Combine(folder, "extended.mod.json"), "{\n  \"schemaVersion\": 1,\n  \"enabled\": true,\n  \"settings\": " + settings + "\n}\n");
        ShroudForgeUi.Load(folder);
        return folder;
    }

    private const string KeySettings = """{ "cleanupKey": { "value": "SDL:89", "label": "Clean up altar zone", "control": "keybind", "applyAt": "live" } }""";

    private static CatalogMod Mod(string folder, string id, SfiRow? sfi = null) =>
        new() { Engine = "shroudforge", Id = id, Name = id, Folder = folder, State = ModRunState.Loaded, Active = true, Sfi = sfi };

    private static ModManagerPage Page(params CatalogMod[] mods) => new()
    {
        Source = () => new CatalogSnapshot
        {
            Version = 1, Mods = mods, Engines = [new CatalogEngine { Kind = "shroudforge", Name = "ShroudForge", Count = mods.Length }],
        },
        Filter = new EngineFilter("shroudforge"), Selected = ModManagerLogic.KeyOf(mods[0]),
    };

    private void Frame(ModManagerPage page)
    {
        _ui.Frame.Number++;
        page.Draw(_ui, new PageRect(0, 0, 1800, 900), 1);
    }

    [Fact]
    public void The_details_of_a_hosted_mod_show_its_newest_notices_and_where_its_keys_work()
    {
        State(Event("altar-cleanup", "result", "Altar Cleanup: 487 blocks found", "Press Keypad 1 again within 60 seconds to clean up.", 100));
        var folder = Package("altar-cleanup", KeySettings);
        ShroudForgeNews.Reload();
        var page = Page(Mod(folder, "altar-cleanup"));
        FakeUiTable.Texts.Clear();
        Frame(page);
        var drawn = string.Join("\n", FakeUiTable.Texts);
        Assert.Contains("Altar Cleanup: 487 blocks found", drawn);
        Assert.Contains("Press Keypad 1 again within 60 seconds to clean up.", drawn);
        Assert.Contains(KilnText.T("shroudforge.news.keys_after_menu", ("key", ShroudForgeMenu.KeyName)), drawn);
    }

    [Fact]
    public void Another_mods_notices_are_not_drawn_in_these_details()
    {
        State(Event("glider", "hello", "Glider news", "Ready to fly", 100));
        var folder = Package("altar-cleanup", KeySettings);
        ShroudForgeNews.Reload();
        var page = Page(Mod(folder, "altar-cleanup"));
        FakeUiTable.Texts.Clear();
        Frame(page);
        var drawn = string.Join("\n", FakeUiTable.Texts);
        Assert.DoesNotContain("Glider news", drawn);
        Assert.DoesNotContain(KilnText.T("shroudforge.news.label"), drawn);
    }

    [Fact]
    public void A_kiln_backend_row_never_reads_ShroudForges_state_file()
    {
        var asked = 0;
        ShroudForgeNews.StatePath = () => { asked++; return StatePath; };
        State(Event("altar-cleanup", "result", "Found", "Press again", 100));
        var folder = Package("altar-cleanup", KeySettings);
        var page = Page(Mod(folder, "altar-cleanup", new SfiRow(SfiBranchKind.Kiln, true, [])));
        FakeUiTable.Texts.Clear();
        Frame(page);
        Frame(page);
        Assert.Equal(0, asked);
        Assert.DoesNotContain("Press again", string.Join("\n", FakeUiTable.Texts));
    }
}
