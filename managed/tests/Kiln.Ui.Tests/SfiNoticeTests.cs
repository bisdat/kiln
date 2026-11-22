// SPDX-License-Identifier: MIT
//
// The persistent do-not-save notice (SfiNotice.cs): what Kiln.Ui reads from kiln-sfi's sfi.json (the schema's writer
// fixtures, schema/fixtures/sfi/state/), when the banner shows and is delivered, what hiding it leaves, the badge, the
// footer line, kiln.saves SAVING, and the named event kiln-sfi's native fallback probes. Texts and delivery counts only:
// no pixels, no game.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.SfiNoticeTests;

[Collection(Kiln.FakeUi.UiSerial.Name)]
public sealed class SfiNoticeTests : IDisposable
{
    private const int Pid = 4242;
    private const long Start = 1763520000000;
    private readonly List<string> _log = [];
    private readonly IKilnBackend _backend = Backend.Current;
    private readonly List<(IReadOnlyList<SfiBannerLine>? Banner, string? Footer)> _painted = [];
    private (ScreenKind Kind, bool InGame, bool Known) _screen = (ScreenKind.MainMenu, false, true);

    public SfiNoticeTests()
    {
        SfiSafetyNotice.ResetForTests();
        Backend.Current = new Kiln.FakeUi.FakeUiTable.Backend();  // no kiln.ui table: Recount and the menu flag do nothing
        SfiSafetyNotice.Log = (level, text) => { lock (_log) _log.Add($"{level}: {text}"); };
        SfiSafetyNotice.DrawingOn = () => false;
        SfiSafetyNotice.NowMs = () => 1000;
        SfiSafetyNotice.ScreenNow = () => _screen;
        SfiSafetyNotice.Paint = (_, banner, footer) => _painted.Add((banner, footer));
    }

    public void Dispose()
    {
        SfiSafetyNotice.ResetForTests();
        SfiSafetyNotice.DrawingOn = () => UiNative.DrawingOffReason == null;
        Backend.Current = _backend;
    }

    private static string Fixtures([CallerFilePath] string self = "")
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Path.GetDirectoryName(self) ?? "" })
            for (var d = new DirectoryInfo(start); d != null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "schema", "fixtures", "sfi", "state", "writer", "running-qs-skipped.json")))
                    return Path.Combine(d.FullName, "schema", "fixtures", "sfi", "state");
        throw new InvalidOperationException("schema/fixtures/sfi/state not found above the test assembly or its source");
    }

    private static JsonNode Fixture(string name) => JsonNode.Parse(File.ReadAllText(Path.Combine(Fixtures(), name)))!;

    private static SfiSafetyView? Read(JsonNode n, int pid = Pid, long start = Start)
    {
        using var doc = JsonDocument.Parse(n.ToJsonString());
        return SfiSafetyReader.Read(doc.RootElement, pid, start);
    }

    private static SfiSafetyView View(string name) => Read(Fixture(name)) ?? throw new InvalidOperationException(name);

    private static JsonObject Package(string id, string intent, string trigger, bool sensitive, string presence = "not-applied",
        bool? archive = false, bool? runtime = false, bool skip = true) => new()
    {
        ["id"] = id, ["version"] = "1.0.0", ["name"] = null, ["intent"] = intent, ["archivePresent"] = archive, ["runtimeActive"] = runtime,
        ["saveSensitive"] = new JsonObject { ["value"] = sensitive, ["basis"] = sensitive ? "unknown-mixed" : "none", ["policyRow"] = null },
        ["skip"] = skip ? new JsonObject { ["code"] = "dll-gate", ["text"] = "refused", ["part"] = "dll", ["chain"] = new JsonArray(), ["late"] = false, ["loader"] = null } : null,
        ["warning"] = new JsonObject { ["revision"] = 1, ["trigger"] = trigger, ["presence"] = presence, ["saveSensitive"] = sensitive, ["text"] = id + " text. Do not save." },
    };

    private static JsonNode With(params JsonObject[] packages)
    {
        var n = Fixture("writer/running-qs-skipped.json");
        n["packages"] = new JsonArray(packages.Select(p => (JsonNode)p).ToArray());
        return n;
    }

    // ---- what sfi.json says

    [Fact]
    public void Quiver_and_Spellbook_skipped_names_the_ammo_loss_and_do_not_save()
    {
        var v = View("writer/running-qs-skipped.json");
        var w = Assert.Single(v.Warnings);  // example-lua runs: no warning
        Assert.Equal(("ammo-slots", "Quiver & Spellbook (ammo-slots 0.8.2)", true, false, SfiPresence.NotApplied, true), (w.Id, w.Label, w.Skipped, w.Late, w.Presence, w.SaveSensitive));
        Assert.Contains("The game drops the quiver and spellbook ammo at the next save while this mod is off.", w.Text);
        Assert.Contains("Do not save while this mod is off; restore it and restart first.", w.Text);
        Assert.Contains("XHL-Core still loads its DLL.", w.Text);
        Assert.Equal("Skipped this start. Not applied: its data is not in the game. Save-sensitive: saving can lose items or saved data.",
            SfiSafetyNotice.StatusLine(w));
        Assert.Equal((1, "3f2b8c1e5a7d4e2f"), (v.WarningRevision, v.LaunchId));
    }

    [Fact]
    public void Late_loss_says_data_still_applied_runtime_stopped()
    {
        var v = View("writer/running-late-loss.json");
        var w = v.Warnings.Single(x => x.Id == "example-lua");
        Assert.Equal(SfiPresence.DataAppliedRuntimeStopped, w.Presence);
        Assert.True(w.Late);
        Assert.StartsWith("Stopped during this start. Startup data still applied, runtime stopped: cannot be undone until restart.", SfiSafetyNotice.StatusLine(w));
        Assert.Contains("Its startup data is still applied, but its runtime stopped; it cannot be undone until restart. Do not save.", w.Text);
        Assert.DoesNotContain("Not applied", SfiSafetyNotice.StatusLine(w));
    }

    [Fact]
    public void Unknown_presence_is_said_as_unknown_never_as_not_applied()
    {
        var w = View("valid/unknown-presence.json").Warnings.Single();
        Assert.Equal(SfiPresence.Unknown, w.Presence);
        Assert.StartsWith("Skipped this start. Unknown: whether its data is in the game could not be verified.", SfiSafetyNotice.StatusLine(w));
    }

    [Theory]
    [InlineData("not-applied", null, false)]           // not proved absent
    [InlineData("not-applied", true, false)]
    [InlineData("data-applied-runtime-stopped", null, false)]
    [InlineData("data-applied-runtime-stopped", true, true)]  // the runtime still runs
    [InlineData("restored-safe", false, false)]        // a word this reader does not know
    public void A_presence_the_record_does_not_back_is_unknown(string presence, bool? archive, bool runtime)
    {
        var v = Read(With(Package("p", "selected", "skipped", false, presence, archive, runtime)))!;
        Assert.Equal(SfiPresence.Unknown, Assert.Single(v.Warnings).Presence);
    }

    [Fact]
    public void Save_sensitive_packages_off_now_warn_disabled_and_absent()
    {
        var v = View("writer/save-sensitive-off.json");
        Assert.Equal(["ammo-slots", "mixed"], v.Warnings.Select(w => w.Id));
        Assert.All(v.Warnings, w => Assert.False(w.Skipped));
        Assert.Equal(SfiPresence.Unknown, v.Warnings[1].Presence);
        Assert.StartsWith("Active at the previous start, off now. Not applied", SfiSafetyNotice.StatusLine(v.Warnings[0]));
        Assert.Contains("The game drops the quiver and spellbook ammo", v.Warnings[0].Text);
    }

    [Fact]
    public void Disabled_and_other_side_rows_never_warn()
    {
        var v = Read(With(
            Package("other", "other-side", "skipped", true),
            Package("other-ss", "other-side", "save-sensitive-off", true, skip: false),
            Package("disabled", "disabled", "skipped", false, skip: false),
            Package("disabled-normal", "disabled", "save-sensitive-off", false, skip: false),
            Package("selected-off", "selected", "save-sensitive-off", true),
            Package("selected-no-skip", "selected", "skipped", false, skip: false),
            Package("real", "selected", "skipped", false)))!;
        Assert.Equal(["real"], v.Warnings.Select(w => w.Id));
    }

    [Fact]
    public void Only_a_kiln_start_of_this_process_counts()
    {
        var n = Fixture("writer/running-qs-skipped.json");
        Assert.NotNull(Read(n));
        Assert.NotNull(Read(n, start: Start + 1500));  // clocks
        Assert.Null(Read(n, pid: 4243));                // another process
        Assert.Null(Read(n, start: Start + 60_000));    // a reused pid
        foreach (var name in new[] { "writer/aside-hosted.json", "writer/aside-carried.json", "writer/aside-older-core.json" })
            Assert.Null(Read(Fixture(name), pid: Fixture(name)["launch"]!["pid"]!.GetValue<int>()));
        var installed = Fixture("writer/running-qs-skipped.json");
        installed["launch"]!["backend"]!["selected"] = "installed";
        Assert.Null(Read(installed));
        var newer = Fixture("writer/running-qs-skipped.json");
        newer["format"] = 2;
        Assert.Null(Read(newer));
    }

    // ---- when the banner shows

    private static SfiSafetyView Rev(long revision, int warnings = 1) =>
        new("3f2b8c1e5a7d4e2f", revision, Enumerable.Range(0, warnings)
            .Select(i => new SfiWarning("p" + i, "P" + i, true, SfiPresence.NotApplied, true, revision, "p text. Do not save.")).ToArray());

    [Fact]
    public void Only_a_package_that_can_lose_data_makes_it_say_do_not_save()
    {
        SfiWarning W(string id, bool sensitive, bool late = false) => new(id, id.ToUpperInvariant(), true, SfiPresence.NotApplied, sensitive, 1, id + " text", late);
        var plain = new SfiSafetyView("3f2b8c1e5a7d4e2f", 1, [W("a", false), W("b", false)]);
        Assert.Equal("2 mods are off this start", SfiSafetyNotice.Banner(plain)[0].Text);
        Assert.Equal("2 mods off this start", SfiSafetyNotice.FooterText(plain));
        Assert.Equal("off this start", SfiSafetyNotice.BadgeFor(plain.Warnings[0]));
        var mixed = new SfiSafetyView("3f2b8c1e5a7d4e2f", 1, [W("a", false), W("q", true)]);
        Assert.Equal("Do not save: 2 mods are off this start", SfiSafetyNotice.Banner(mixed)[0].Text);
        Assert.Equal("2 mods off: do not save", SfiSafetyNotice.FooterText(mixed));
        Assert.Equal("off this start", SfiSafetyNotice.BadgeFor(mixed.Warnings[0]));
        Assert.Equal("do not save", SfiSafetyNotice.BadgeFor(mixed.Warnings[1]));
        var late = new SfiSafetyView("3f2b8c1e5a7d4e2f", 1, [W("l", false, late: true)]);
        Assert.Equal("Do not save: 1 mod is off this start", SfiSafetyNotice.Banner(late)[0].Text);  // a late loss: data applied, runtime stopped
        Assert.Equal("1 mod off: do not save", SfiSafetyNotice.FooterText(late));
        Assert.Equal("1 mod off this start", MainMenuFooter.WarningLine(1, doNotSave: false));
    }

    [Fact]
    public void The_earliest_main_menu_frame_shows_it_and_only_a_drawn_frame_delivers_it()
    {
        var m = SfiSafetyNotice.Model;
        Assert.True(m.Publish(Rev(1)));
        Assert.True(m.BannerUp);
        Assert.Empty(m.Shown);  // read and armed is not delivered
        _screen = (ScreenKind.Title, false, true);
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        Assert.Empty(_painted);  // the title screen is not a usable menu
        Assert.Empty(m.Shown);
        _screen = (ScreenKind.MainMenu, false, true);
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        var (banner, footer) = Assert.Single(_painted);
        Assert.Equal("Do not save: 1 mod is off this start", banner![0].Text);
        Assert.Equal("1 mod off: do not save", footer);
        Assert.Equal(new SfiNoticeShown(1, "start", 0, 1000), Assert.Single(m.Shown));
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        Assert.Single(m.Shown);  // one delivery per showing
        Assert.Contains(_log, l => l.Contains("banner shown (warning revision 1, start, world session 0)"));
    }

    [Fact]
    public void A_frame_that_faults_delivers_nothing_and_the_banner_stays_up()
    {
        SfiSafetyNotice.Model.Publish(Rev(1));
        SfiSafetyNotice.Paint = (_, _, _) => throw new InvalidOperationException("boom");
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        Assert.Empty(SfiSafetyNotice.Model.Shown);
        Assert.True(SfiSafetyNotice.Model.BannerUp);
        Assert.Contains(_log, l => l.StartsWith("Error: drawing failed: boom"));
        SfiSafetyNotice.Paint = (_, banner, footer) => _painted.Add((banner, footer));
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        Assert.Single(SfiSafetyNotice.Model.Shown);
    }

    [Fact]
    public void Hiding_hides_only_the_banner()
    {
        var m = SfiSafetyNotice.Model;
        m.Publish(View("writer/running-qs-skipped.json"));
        m.Dismiss();
        Assert.False(m.BannerUp);
        Assert.True(m.Warns);
        Assert.Single(m.View!.Warnings);
        Assert.Equal(1u, SfiSafetyNotice.Demand);
        Assert.NotNull(SfiSafetyNotice.Badge(new CatalogMod { Engine = "shroudforge", Id = "AMMO-SLOTS", Name = "Q&S" }));
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        var (banner, footer) = Assert.Single(_painted);
        Assert.Null(banner);                        // no banner ...
        Assert.Equal("1 mod off: do not save", footer);  // ... the footer line stays
        Assert.Empty(m.Shown);
    }

    [Fact]
    public void Entering_a_world_shows_it_again()
    {
        var m = SfiSafetyNotice.Model;
        m.Publish(Rev(1));
        m.Dismiss();
        Assert.False(m.ScreenChanged(true, true));   // already in the world
        Assert.False(m.ScreenChanged(true, false));  // leaving
        Assert.False(m.BannerUp);
        Assert.True(m.ScreenChanged(false, true));
        Assert.True(m.BannerUp);
        _screen = (ScreenKind.InGame, true, true);
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        Assert.Null(_painted.Single().Footer);  // the footer line is the main menu's
        Assert.Equal(new SfiNoticeShown(1, "world", 1, 1000), m.Shown.Single());
    }

    [Fact]
    public void A_new_warning_revision_shows_it_again_the_same_one_does_not()
    {
        var m = SfiSafetyNotice.Model;
        m.Publish(Rev(1));
        m.Dismiss();
        m.Publish(Rev(1));
        Assert.False(m.BannerUp);
        m.Publish(Rev(2, 2));
        Assert.True(m.BannerUp);
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        Assert.Equal(new SfiNoticeShown(2, "revision", 0, 1000), m.Shown.Single());
        Assert.Equal("2 mods off: do not save", _painted.Single().Footer);
    }

    [Fact]
    public void Each_kiln_saves_SAVING_shows_it_again_other_events_and_history_do_not()
    {
        SfiSafetyNotice.Model.Publish(Rev(1));
        SfiSafetyNotice.Model.Dismiss();
        var events = new List<(uint Kind, ulong Seq)> { (1, 7), (2, 8), (5, 9), (3, 10) };  // history up to 10
        SfiSafetyNotice.NextSave = after => events.FirstOrDefault(e => e.Seq > after) is { Seq: > 0 } e ? e : null;
        SfiSafetyNotice.SaveSeqForTests = 10;  // what kiln.saves said when the worker began
        SfiSafetyNotice.PollSaves();
        Assert.False(SfiSafetyNotice.Model.BannerUp);
        events.Add((2, 11));  // WRITTEN
        SfiSafetyNotice.PollSaves();
        Assert.False(SfiSafetyNotice.Model.BannerUp);
        events.Add((1, 12));  // SAVING
        SfiSafetyNotice.PollSaves();
        Assert.True(SfiSafetyNotice.Model.BannerUp);
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        Assert.Equal("saving", SfiSafetyNotice.Model.Shown.Single().Why);
        SfiSafetyNotice.Model.Dismiss();
        SfiSafetyNotice.PollSaves();  // nothing new
        Assert.False(SfiSafetyNotice.Model.BannerUp);
        events.Add((1, 13));
        SfiSafetyNotice.PollSaves();
        Assert.True(SfiSafetyNotice.Model.BannerUp);
        Assert.Equal(13ul, SfiSafetyNotice.SaveSeqForTests);
    }

    [Fact]
    public void Without_warnings_nothing_shows_and_nothing_is_demanded()
    {
        var m = SfiSafetyNotice.Model;
        Assert.Equal(0u, SfiSafetyNotice.Demand);
        Assert.False(m.ScreenChanged(false, true));
        Assert.False(m.Saving(1));
        m.Publish(Rev(0, 0));
        Assert.False(m.BannerUp);
        Assert.Equal(0u, SfiSafetyNotice.Demand);
        Assert.Null(SfiSafetyNotice.FooterText(m.View));
        Assert.Null(SfiSafetyNotice.Badge(new CatalogMod { Engine = "shroudforge", Id = "example-lua", Name = "x" }));
        SfiSafetyNotice.Draw(UiEngine.DrawContext);
        Assert.Empty(_painted);
    }

    [Fact]
    public void The_badge_is_on_the_warned_ShroudForge_row_only()
    {
        SfiSafetyNotice.Model.Publish(View("writer/running-late-loss.json"));
        Assert.Equal("example-lua", SfiSafetyNotice.Badge(new CatalogMod { Engine = "shroudforge", Id = "example-lua", Name = "x" })?.Id);
        Assert.Null(SfiSafetyNotice.Badge(new CatalogMod { Engine = "kiln", Id = "example-lua", Name = "x" }));
        Assert.Null(SfiSafetyNotice.Badge(new CatalogMod { Engine = "shroudforge", Id = "other", Name = "x" }));
    }

    [Fact]
    public void The_banner_lists_the_first_packages_and_points_to_the_whole_text()
    {
        var lines = SfiSafetyNotice.Banner(Rev(3, 6));
        Assert.Equal("Do not save: 6 mods are off this start", lines[0].Text);
        Assert.Equal(1 + 3 * SfiSafetyNotice.BannerPackages + 2, lines.Count);
        Assert.Equal("and 2 more", lines[^2].Text);
        Assert.Contains("Kiln's Mods page", lines[^1].Text);
        var longText = new string('x', 1000);
        var one = SfiSafetyNotice.Banner(new SfiSafetyView(null, 1, [new SfiWarning("a", "A", true, SfiPresence.Unknown, false, 1, longText)]));
        Assert.Equal(SfiSafetyNotice.BannerBodyChars + 3, one[3].Text.Length);
        Assert.EndsWith("...", one[3].Text);
    }

    [Fact]
    public void Neither_the_footer_setting_nor_the_folder_notes_preference_suppresses_it()
    {
        // the footer line comes from the view alone, whatever mainMenuFooter (GameMenuEntries.FooterShown) says
        SfiSafetyNotice.Model.Publish(Rev(1));
        Assert.Equal("1 mod off: do not save", SfiSafetyNotice.FooterText(SfiSafetyNotice.Model.View));
        Assert.Equal("4 mods off: do not save", MainMenuFooter.WarningLine(4));
        var source = File.ReadAllText(Path.Combine(Fixtures(), "..", "..", "..", "..", "managed", "services", "Kiln.Ui", "SfiNotice.cs"));
        Assert.DoesNotContain("FooterShown", source);
        Assert.DoesNotContain("modFolderNotes", source, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("FolderNotes", source, StringComparison.OrdinalIgnoreCase);
    }

    // ---- the worker's file read and Apply

    [Fact]
    public void The_worker_reads_the_file_and_a_torn_or_missing_file_is_not_a_kiln_start()
    {
        var dir = Directory.CreateTempSubdirectory("kiln-sfi-notice-");
        try
        {
            var path = Path.Combine(dir.FullName, "sfi.json");
            Assert.Null(SfiSafetyNotice.ReadFile(path, Pid, Start));
            File.WriteAllText(path, "{\"format\":1,");
            Assert.Null(SfiSafetyNotice.ReadFile(path, Pid, Start));
            File.WriteAllText(path, Fixture("writer/running-qs-skipped.json").ToJsonString());
            var v = SfiSafetyNotice.ReadFile(path, Pid, Start)!;
            SfiSafetyNotice.Apply(v);
            Assert.True(SfiSafetyNotice.Model.BannerUp);
            Assert.Contains(_log, l => l.StartsWith("Warn: 1 package(s) off this start (warning revision 1): ammo-slots"));
        }
        finally { dir.Delete(true); }
    }

    [Fact]
    public void The_worker_reads_with_delete_sharing_and_a_byte_bound()
    {
        var dir = Directory.CreateTempSubdirectory("kiln-sfi-notice-");
        try
        {
            var path = Path.Combine(dir.FullName, "sfi.json");
            var next = Path.Combine(dir.FullName, "sfi.json.tmp");
            var fixture = Fixture("writer/running-qs-skipped.json").ToJsonString();
            File.WriteAllText(path, fixture);
            File.WriteAllText(next, fixture);
            // kiln-sfi replaces the file by rename while the worker has it open. On Windows that rename is refused while
            // any handle on the old file is open, whatever its sharing (kiln-sfi's writer retries for about half a
            // second): the read must not fail for it, and must hold no handle once it returns, so the writer's next
            // try goes through. Elsewhere the rename goes through at once.
            string? during = null;
            var opens = 0;
            SfiSafetyNotice.OnOpenForTests = _ =>
            {
                if (opens++ > 0) return;
                try { File.Move(next, path, overwrite: true); during = "renamed"; }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { during = e.GetType().Name + ": " + e.Message; }
            };
            Assert.NotNull(SfiSafetyNotice.ReadFile(path, Pid, Start));
            Assert.Equal(1, opens);
            SfiSafetyNotice.OnOpenForTests = null;
            if (OperatingSystem.IsWindows())
            {
                Assert.True(during != "renamed" && File.Exists(next), "error unexpected on Windows: the rename went through during the read");
                File.Move(next, path, overwrite: true);  // the read is over: nothing holds the file
            }
            else Assert.True(during == "renamed", "error " + during);
            Assert.False(File.Exists(next));

            // the bound holds on what is read, not on the length seen before opening: a file growing past it is refused
            var len = (int)new FileInfo(path).Length;
            Assert.NotNull(SfiSafetyNotice.ReadBytes(path, len));
            Assert.Null(SfiSafetyNotice.ReadBytes(path, len - 1));
            SfiSafetyNotice.OnOpenForTests = fs =>
            {
                using var w = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
                w.Write(new byte[64]);
            };
            Assert.Null(SfiSafetyNotice.ReadBytes(path, len + 16));
            SfiSafetyNotice.OnOpenForTests = null;
            Assert.Null(SfiSafetyNotice.ReadBytes(path, len + 16));  // grown: refused before it is opened
            Assert.Null(SfiSafetyNotice.ReadFile(Path.Combine(dir.FullName, "missing.json"), Pid, Start));
            File.WriteAllText(path, "");
            Assert.Null(SfiSafetyNotice.ReadBytes(path));
        }
        finally
        {
            SfiSafetyNotice.OnOpenForTests = null;
            dir.Delete(true);
        }
    }

    // ---- the attention marker on Kiln's own Mods entry

    private static readonly List<(ulong Entry, string Text, uint Flags)> MenuSets = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RecordMenuSet(ulong entry, NativeStr text, uint flags)
    {
        lock (MenuSets) MenuSets.Add((entry, text.ToString(), flags));
        return 0;
    }

    [Fact]
    public unsafe void The_attention_marker_and_a_mods_hidden_Kiln_entry_keep_each_other()
    {
        // menu_set replaces every flag of the entry: the marker must not show an entry a mod hid, and a mod setting
        // Ui.Menus.KilnEntryVisible must not clear the marker
        Kiln.FakeUi.FakeUiTable.Install();
        Backend.Current = new Kiln.FakeUi.FakeUiTable.Backend();
        UiNative.ResetForTests();
        try
        {
            Kiln.FakeUi.FakeUiTable.Table->Base.Base.Base.MenuSet = &RecordMenuSet;
            SfiNoticeMenuShim.KilnEntry.Visible = true;
            GameMenuEntries.SafetyAttention(false);
            lock (MenuSets) MenuSets.Clear();
            SfiNoticeMenuShim.KilnEntry.Visible = false;
            GameMenuEntries.SafetyAttention(true);
            Assert.Equal(1u | GameMenuEntries.AttentionFlag, MenuSets[^1].Flags);  // still hidden, marked
            SfiNoticeMenuShim.KilnEntry.Visible = true;
            Assert.Equal(GameMenuEntries.AttentionFlag, MenuSets[^1].Flags);       // shown, still marked
            GameMenuEntries.SafetyAttention(false);
            Assert.Equal(0u, MenuSets[^1].Flags);
            Assert.All(MenuSets, c => Assert.Equal(0ul, c.Entry));                // Kiln's own entries
        }
        finally
        {
            Backend.Current = new Kiln.FakeUi.FakeUiTable.Backend();
            Kiln.FakeUi.FakeUiTable.Free();
            UiNative.ResetForTests();
            lock (MenuSets) MenuSets.Clear();
        }
    }

    // ---- the named event kiln-sfi's native fallback probes

    [Fact]
    public void The_ready_event_exists_once_Kiln_Ui_is_up_in_a_kiln_start()
    {
        var pid = Environment.ProcessId;
        Assert.Equal($"Local\\Kiln.Sfi.UiReady.{pid}", SfiSafetyNotice.ReadyEventName(pid));
        if (!OperatingSystem.IsWindows())
        {
            Assert.False(SfiSafetyNotice.EnsureReady(pid, true, true));
            return;
        }
        Assert.False(Exists(pid));
        Assert.False(SfiSafetyNotice.EnsureReady(pid, false, true));  // hosted / installed: no event
        Assert.False(SfiSafetyNotice.EnsureReady(pid, true, false));  // overlay drawing off: kiln-sfi's fallback decides
        Assert.False(Exists(pid));
        Assert.True(SfiSafetyNotice.EnsureReady(pid, true, true));
        Assert.True(Exists(pid));
        Assert.True(SfiSafetyNotice.EnsureReady(pid, true, true));  // once
        var h = OpenEventW(Synchronize, false, SfiSafetyNotice.ReadyEventName(pid));
        try { Assert.Equal(0u, WaitForSingleObject(h, 0)); }  // signalled: a probe may wait on it too
        finally { CloseHandle(h); }
        SfiSafetyNotice.ResetForTests();  // the holder closes it: the name is gone
        Assert.False(Exists(pid));
    }

    [Fact]
    public void Apply_creates_the_ready_event_only_when_Kiln_Ui_can_draw()
    {
        if (!OperatingSystem.IsWindows()) return;
        var pid = Environment.ProcessId;
        SfiSafetyNotice.Apply(Rev(1));  // DrawingOn false in these tests
        Assert.False(Exists(pid));
        SfiSafetyNotice.DrawingOn = () => true;
        SfiSafetyNotice.Apply(Rev(1));
        Assert.True(Exists(pid));
    }

    [Fact]
    public void A_ready_event_someone_made_unsignalled_is_kept_and_signalled()
    {
        if (!OperatingSystem.IsWindows()) return;
        var pid = Environment.ProcessId;
        var squatter = CreateEventW(0, true, false, SfiSafetyNotice.ReadyEventName(pid));
        Assert.NotEqual(0, squatter);
        try
        {
            Assert.Equal(WaitTimeout, WaitForSingleObject(squatter, 0));  // unsignalled
            Assert.True(SfiSafetyNotice.EnsureReady(pid, true, true));
            Assert.Equal(0u, WaitForSingleObject(squatter, 0));  // signalled now, as Kiln.Ui's own event starts
            Assert.Contains(_log, l => l.Contains("already existed") && l.Contains("signalled it"));
        }
        finally { CloseHandle(squatter); }
    }

    [Fact]
    public void A_ready_event_that_cannot_be_made_is_logged_once_and_tried_again()
    {
        if (!OperatingSystem.IsWindows()) return;
        var pid = Environment.ProcessId;
        var mutex = CreateMutexW(0, false, SfiSafetyNotice.ReadyEventName(pid));  // the name held by another object type
        Assert.NotEqual(0, mutex);
        try
        {
            for (var i = 0; i < 5; i++) Assert.False(SfiSafetyNotice.EnsureReady(pid, true, true));  // Apply: every 2 s
            Assert.Single(_log, l => l.Contains("not created"));
        }
        finally { CloseHandle(mutex); }
        Assert.True(SfiSafetyNotice.EnsureReady(pid, true, true));  // the name is free again: made now
        Assert.True(Exists(pid));
    }

    private static bool Exists(int pid)
    {
        var h = OpenEventW(Synchronize, false, SfiSafetyNotice.ReadyEventName(pid));
        if (h == 0)
        {
            Assert.Equal(2, Marshal.GetLastPInvokeError());  // ERROR_FILE_NOT_FOUND
            return false;
        }
        CloseHandle(h);
        return true;
    }

    private const uint Synchronize = 0x00100000;
    private const uint WaitTimeout = 0x102;

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern nint CreateEventW(nint attributes, bool manualReset, bool initialState, string name);

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern nint CreateMutexW(nint attributes, bool initialOwner, string name);

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern nint OpenEventW(uint access, bool inherit, string name);

    [DllImport("kernel32")]
    private static extern bool CloseHandle(nint h);

    [DllImport("kernel32")]
    private static extern uint WaitForSingleObject(nint h, uint ms);
}
