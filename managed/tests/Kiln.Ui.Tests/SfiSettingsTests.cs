// SPDX-License-Identifier: MIT
//
// The switch and settings of a ShroudForge mod in a kiln-backend start, drawn through the real page code
// (ModManagerPage.Draw, ShroudForgeUi) against a fake kiln.ui table: the same byte-preserving writer on the package's own
// extended.mod.json under <game>/mods, every change said to apply at the next start, an enable request that grants
// nothing, a failed save that is not shown as off; and the Kiln-owned loader settings document. No game.
using System.Text.Json;
using System.Text.Json.Nodes;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.SfiSettingsTests;

[Collection(UiSerial.Name)]
public sealed class SfiSettingsTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-sfi-settings-" + Guid.NewGuid().ToString("N"));
    private readonly UiDraw _ui = new();
    private readonly List<string> _sfRoot = [];
    private string Kiln => Path.Combine(_root, "kiln");
    private string Mods => Path.Combine(_root, "mods");

    public SfiSettingsTests()
    {
        _previous = Backend.Current;
        FakeUiTable.Install();
        Backend.Current = new FakeUiTable.Backend();
        UiNative.ResetForTests();
        ShroudForgeUi.ResetForTests();
        SfiBridge.ResetForTests();
        _ui.Frame.DpiScale = 1;
        _ui.Frame.Width = 1920;
        _ui.Frame.Height = 1080;
        Directory.CreateDirectory(Path.Combine(Kiln, "state"));
        ModCatalog.Sources = () => new CatalogSources { KilnRoot = Kiln };
        ShroudForgePackages.OnSfRoot = (game, key) =>
        {
            if (ModCatalog.SamePath(game, _root)) lock (_sfRoot) _sfRoot.Add(key);
        };
    }

    public void Dispose()
    {
        ShroudForgePackages.OnSfRoot = null;
        ShroudForgeUi.SetSetting = ShroudForgePackages.SetSetting;
        SfiBridge.ResetForTests();
        ShroudForgeUi.ResetForTests();
        ModCatalog.Sources = ModCatalog.DefaultSources;
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
        try { Directory.Delete(_root, true); }
        catch (IOException) { }
    }

    private const string LiveSettings = """{ "speed": { "value": 1.50, "label": "Speed", "applyAt": "live" }, "note": { "value": "a", "label": "Note" } }""";

    private string Package(string id = "notes", bool on = true)
    {
        var folder = SfiCatalogTests.SfiCatalogTests.Package(Path.Combine(Mods, id), id, on: on, settings: LiveSettings);
        ShroudForgeUi.Load(folder);
        return folder;
    }

    private static SfiRow KilnRow(params (string, bool)[] lines) => new(SfiBranchKind.Kiln, true, lines);

    private static CatalogMod Mod(string folder, SfiRow? sfi, bool active = true, ModRunState state = ModRunState.Loaded) =>
        new() { Engine = "shroudforge", Id = "notes", Name = "Notes", Folder = folder, State = state, Active = active, Sfi = sfi };

    private static ModManagerPage Page(CatalogMod mod) => new()
    {
        Source = () => new CatalogSnapshot
        {
            Version = 1, Mods = [mod], Engines = [new CatalogEngine { Kind = "shroudforge", Name = "ShroudForge", Count = 1 }],
        },
        Filter = new EngineFilter("shroudforge"), Selected = "shroudforge/notes",
    };

    private List<string> Frame(ModManagerPage page)
    {
        lock (FakeUiTable.Texts) FakeUiTable.Texts.Clear();
        _ui.Frame.Number++;
        page.Draw(_ui, new PageRect(0, 0, 1800, 900), 1);
        FakeUiTable.Pressed.Clear();
        lock (FakeUiTable.Texts) return [.. FakeUiTable.Texts];
    }

    private int SfRootHits() { lock (_sfRoot) return _sfRoot.Count; }

    private static string Text(string key, params (string, object?)[] args) => KilnText.T(key, args);

    /// <summary>A runtime package with a live setting: hosted says ShroudForge applies it while you play; the same package
    /// as a kiln-backend row says every switch and setting change applies at the next start, and shows the interpreter's
    /// lines. The save line follows the row too.</summary>
    [Fact]
    public void A_kiln_row_says_every_change_applies_at_the_next_start()
    {
        var folder = Package();
        var hosted = Frame(Page(Mod(folder, null)));
        Assert.Contains(Text("shroudforge.switch_live"), hosted);
        Assert.DoesNotContain(Text("shroudforge.next_start"), hosted);  // a runtime package: both settings apply live
        Assert.DoesNotContain(ShroudForgeUi.SfiNextStartNote, hosted);

        var kiln = Frame(Page(Mod(folder, KilnRow(("This start: Game data: none. Lua: active.", false)))));
        Assert.Contains(Text("shroudforge.switch_next"), kiln);
        Assert.DoesNotContain(Text("shroudforge.switch_live"), kiln);
        Assert.Equal(2, kiln.Count(t => t == Text("shroudforge.next_start")));  // both, live as they are
        Assert.Contains(ShroudForgeUi.SfiNextStartNote, kiln);
        Assert.Contains("This start: Game data: none. Lua: active.", kiln);

        ShroudForgeUi.Edit(folder, null, JsonValue.Create(false));
        ShroudForgeUi.Flush();
        Assert.Contains(Text("shroudforge.save.switch_next_off", ("mod", "Name notes")), Frame(Page(Mod(folder, KilnRow()))));
        ShroudForgeUi.Edit(folder, "speed", JsonValue.Create(2.5));
        ShroudForgeUi.Flush();
        Assert.Contains(Text("shroudforge.save.setting_next", ("setting", "Speed")), Frame(Page(Mod(folder, KilnRow()))));

        // drawn as a hosted row again: ShroudForge's own words come back
        Frame(Page(Mod(folder, null)));
        ShroudForgeUi.Edit(folder, "speed", JsonValue.Create(3.5));
        ShroudForgeUi.Flush();
        Assert.Contains(Text("shroudforge.save.setting_live", ("setting", "Speed")), Frame(Page(Mod(folder, null))));
    }

    /// <summary>In kiln mode a setting change goes to the package's own extended.mod.json under &lt;game&gt;/mods with the
    /// same byte-preserving writer (the other values' tokens, 1.50, stay as written), and nothing reaches
    /// &lt;game&gt;/shroudforge or the interpreter's bridge folder.</summary>
    [Fact]
    public void A_setting_lands_in_the_packages_own_file_with_its_other_tokens_kept()
    {
        var folder = Package();
        Directory.CreateDirectory(Path.Combine(_root, "shroudforge"));
        var page = Page(Mod(folder, KilnRow()));
        Frame(page);
        var before = File.ReadAllText(Path.Combine(folder, "extended.mod.json"));
        ShroudForgeUi.Edit(folder, "note", JsonValue.Create("b"));
        ShroudForgeUi.Flush();
        var after = File.ReadAllText(Path.Combine(folder, "extended.mod.json"));
        Assert.Equal(before.Replace("\"value\": \"a\"", "\"value\": \"b\""), after);
        Assert.Contains("\"value\": 1.50", after);
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(_root, "shroudforge")));
        Assert.False(Directory.Exists(Path.Combine(_root, "kiln", "sfi")));
        Assert.Equal(0, SfRootHits());
    }

    /// <summary>A package off before the start, switched on from the page: only its file's switch changes. The catalog
    /// still reads it as not running, Active false, "Switched on for the next start": no request, no admission.</summary>
    [Fact]
    public void An_enable_request_grants_nothing_this_start()
    {
        var folder = Package(on: false);
        var state = Path.Combine(Kiln, "state");
        var start = DateTime.UtcNow.AddMinutes(-30);
        var startMs = new DateTimeOffset(start).ToUnixTimeMilliseconds();
        var record = SfiCatalogTests.SfiCatalogTests.Record("running",
            [SfiCatalogTests.SfiCatalogTests.Pkg("notes", intent: "disabled", lua: "not-needed", runtime: false, epoch: null)], startMs: startMs);
        File.WriteAllText(Path.Combine(state, "sfi.json"), record.ToJsonString());
        var src = new CatalogSources { KilnRoot = Kiln, Pid = 4242, ProcessStartUtc = start, Side = "client" };
        var row = ModCatalog.Read(src).Mods.Single(m => m.Id == "notes");
        Assert.Equal((ModRunState.Disabled, false), (row.State, row.Active));

        Frame(Page(row));
        ShroudForgeUi.Edit(folder, null, JsonValue.Create(true));
        ShroudForgeUi.Flush();
        Assert.True(ShroudForgePackages.Read(folder).Enabled);
        var again = ModCatalog.Read(src).Mods.Single(m => m.Id == "notes");
        Assert.Equal((ModRunState.Disabled, false), (again.State, again.Active));
        Assert.Contains("Switched on for the next start", again.Help);
        Assert.False(Directory.Exists(Path.Combine(_root, "kiln", "sfi")));  // no bridge request of any kind
        Assert.Equal(record.ToJsonString(), File.ReadAllText(Path.Combine(state, "sfi.json")));
        Assert.Equal(0, SfRootHits());
        // the button of an action stays faint: not Active, so a click says "not running"
        Assert.False(again.Active);
    }

    /// <summary>A save that changed the file and then failed: the page says so and offers no controls of the old package;
    /// the switch is never shown as off because a write was attempted.</summary>
    [Fact]
    public void A_failed_save_is_not_shown_as_off()
    {
        var folder = Package();
        ShroudForgeUi.SetSetting = (f, key, value) =>
        {
            File.AppendAllText(Path.Combine(f, "extended.mod.json"), "{ broken");
            throw new IOException("disk full");
        };
        var page = Page(Mod(folder, KilnRow()));
        Frame(page);
        ShroudForgeUi.Edit(folder, "note", JsonValue.Create("b"));
        ShroudForgeUi.Flush();
        var texts = Frame(page);
        Assert.Contains(texts, t => t.Contains("cannot be read back", StringComparison.Ordinal));
        Assert.DoesNotContain(Text("shroudforge.off"), texts);
    }

    // ------------------------------------------------------------------------------- loader settings ----

    /// <summary>No file means ShroudForge's defaults and nothing written; the first change writes the document under
    /// kiln/config/sfi/loader.json (never &lt;game&gt;/shroudforge); refused values and an unusable file leave it as it was.</summary>
    [Fact]
    public void Loader_settings_live_in_kilns_own_document()
    {
        var path = SfiLoaderSettings.PathOf(Kiln);
        Assert.Equal(Path.Combine(Kiln, "config", "sfi", "loader.json"), path);
        var (net, exists, error) = SfiLoaderSettings.Read(Kiln);
        Assert.False(exists);
        Assert.Null(error);
        Assert.Equal("""{"enabled":true,"serverSteamId":"","allowedClientSteamIds":""}""", net!.ToJsonString());
        Assert.False(File.Exists(path));

        SfiLoaderSettings.Set(Kiln, "serverSteamId", JsonValue.Create("76561198000000099"));
        var doc = JsonNode.Parse(File.ReadAllText(path))!;
        Assert.Equal(1, doc["schemaVersion"]!.GetValue<int>());
        Assert.Equal("76561198000000099", doc["modules"]!["network"]!["serverSteamId"]!.GetValue<string>());
        Assert.True(doc["modules"]!["network"]!["enabled"]!.GetValue<bool>());
        Assert.False(Directory.Exists(Path.Combine(_root, "shroudforge")));

        var written = File.ReadAllText(path);
        foreach (var (key, value) in new (string, JsonNode?)[]
                 {
                     ("serverSteamId", JsonValue.Create("123")), ("serverSteamId", JsonValue.Create(76561198000000099)),
                     ("enabled", JsonValue.Create("yes")), ("allowedClientSteamIds", JsonValue.Create(new string('1', 4097))),
                     ("modloaderUi", JsonValue.Create(true)),
                 })
        {
            Assert.Throws<SfEditException>(() => SfiLoaderSettings.Set(Kiln, key, value));
            Assert.Equal(written, File.ReadAllText(path));
        }
        Assert.Empty(Directory.GetFiles(Path.GetDirectoryName(path)!, "*.tmp"));

        File.WriteAllText(path, """{"schemaVersion":1,"modules":{"network":{"serverSteamId":"x"}}}""");
        Assert.NotNull(SfiLoaderSettings.Read(Kiln).Error);
        Assert.Throws<SfEditException>(() => SfiLoaderSettings.Set(Kiln, "enabled", JsonValue.Create(false)));
        Assert.Equal("""{"schemaVersion":1,"modules":{"network":{"serverSteamId":"x"}}}""", File.ReadAllText(path));

        if (!OperatingSystem.IsWindows())
        {
            var target = Path.Combine(_root, "target.json");
            File.WriteAllText(target, """{"schemaVersion":1,"modules":{"network":{}}}""");
            File.Delete(path);
            File.CreateSymbolicLink(path, target);
            Assert.NotNull(SfiLoaderSettings.Read(Kiln).Error);
            Assert.Throws<SfEditException>(() => SfiLoaderSettings.Set(Kiln, "enabled", JsonValue.Create(false)));
            Assert.Equal("""{"schemaVersion":1,"modules":{"network":{}}}""", File.ReadAllText(target));
        }
    }

    /// <summary>A kiln/config that is a link: the first change is refused, and nothing is made inside the link's target
    /// (no sfi folder, no temp file), so a refusal leaves every folder as it was.</summary>
    [Fact]
    public void A_linked_config_folder_gets_nothing_made_in_its_target()
    {
        if (OperatingSystem.IsWindows()) return;  // a symbolic link needs a privilege there; the check is the same reparse test
        var target = Path.Combine(_root, "elsewhere");
        Directory.CreateDirectory(target);
        Directory.CreateDirectory(Kiln);
        Directory.CreateSymbolicLink(Path.Combine(Kiln, "config"), target);
        Assert.Throws<SfEditException>(() => SfiLoaderSettings.Set(Kiln, "enabled", JsonValue.Create(false)));
        Assert.Empty(Directory.EnumerateFileSystemEntries(target));
    }
}
