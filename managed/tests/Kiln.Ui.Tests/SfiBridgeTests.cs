// SPDX-License-Identifier: MIT
//
// The Kiln menu's ShroudForge action buttons in a kiln-backend start (SfiBridge.cs and its branch in ShroudForgeUi.Action),
// drawn through the real page code (ModManagerPage.Draw) against a fake kiln.ui table, with kiln/state/sfi.json made
// from the shared state fixtures (schema/fixtures/sfi/state) plus the per-package "ownerEpoch" (the
// envelope's ownerEpoch of the package's live runtime; absent = no live runtime). And the notice reader's refusal of
// links and non-regular files (SfiBridgeFiles.ReadNotices). No game, no runtime.
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.SfiBridgeTests;

internal static class SfiFixtures
{
    // above the test assembly first (a copied build on a runner), then above this source file
    public static string Root([CallerFilePath] string self = "")
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Path.GetDirectoryName(self) ?? "" })
            for (var d = new DirectoryInfo(start); d != null; d = d.Parent)
                if (Directory.Exists(Path.Combine(d.FullName, "schema", "fixtures", "sfi", "state", "valid")))
                    return Path.Combine(d.FullName, "schema", "fixtures", "sfi");
        throw new InvalidOperationException("schema/fixtures/sfi not found above the test assembly or its source");
    }

    public static bool TryLink(string link, string target, bool folder)
    {
        try
        {
            if (folder) Directory.CreateSymbolicLink(link, target);
            else File.CreateSymbolicLink(link, target);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or PlatformNotSupportedException) { return false; }
    }
}

[Collection(UiSerial.Name)]
public sealed class SfiBridgeTests : IDisposable
{
    private const string L = "3f2b8c1e5a7d4e2f";  // the fixture's launch
    private static readonly string Plan = new('a', 64);
    private readonly IKilnBackend _previous;
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-sfi-actions-" + Guid.NewGuid().ToString("N"));
    private readonly UiDraw _ui = new();
    private readonly List<(string Game, SfPackage Package, string Action)> _hosted = [];
    private string? _lastLine;

    public SfiBridgeTests()
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
        Directory.CreateDirectory(_root);
        ModCatalog.Sources = () => new CatalogSources { KilnRoot = Path.Combine(_root, "kiln") };
        // the hosted hand-off, recorded and then done as before
        ShroudForgeUi.QueueAction = (game, package, action) =>
        {
            lock (_hosted) _hosted.Add((game, package, action));
            ShroudForgePackages.QueueAction(game, package, action);
        };
    }

    public void Dispose()
    {
        SfiBridge.ResetForTests();
        ShroudForgeUi.ResetForTests();
        ShroudForgeUi.QueueAction = ShroudForgePackages.QueueAction;
        ModCatalog.Sources = ModCatalog.DefaultSources;
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
        try { Directory.Delete(_root, true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    // ---- the files

    private string Game => _root;
    private string StatePath => Path.Combine(_root, "kiln", "state", "sfi.json");
    private string KilnActions => SfiBridgeFiles.ActionsDir(Game, L);
    private string SfActions => Path.Combine(_root, "shroudforge", "ui", "actions");
    private static string[] Files(string dir) => Directory.Exists(dir) ? Directory.EnumerateFiles(dir).Order(StringComparer.Ordinal).ToArray() : [];
    private string[] AnyBridgeFiles() =>
        Directory.Exists(SfiBridgeFiles.UiRoot(Game)) ? Directory.EnumerateFiles(SfiBridgeFiles.UiRoot(Game), "*", SearchOption.AllDirectories).ToArray() : [];

    /// <summary>The running kiln start of the fixture running-qs-skipped.json, made this process's, with "notes" as its
    /// first package: Lua active at owner epoch 3.</summary>
    private static JsonObject KilnState()
    {
        var o = JsonNode.Parse(File.ReadAllText(Path.Combine(SfiFixtures.Root(), "state", "valid", "running-qs-skipped.json")))!.AsObject();
        o["launch"]!["pid"] = Environment.ProcessId;
        var notes = o["packages"]![0]!.AsObject();
        notes["id"] = "notes";
        notes["name"] = "Notes";
        notes["ownerEpoch"] = 3;
        return o;
    }

    private void State(Func<JsonObject, JsonObject>? change = null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(StatePath)!);
        var o = KilnState();
        if (change is not null) o = change(o);
        File.WriteAllText(StatePath, o.ToJsonString());
    }

    private static JsonObject Notes(JsonObject o) => o["packages"]![0]!.AsObject();

    private string ActionPackage(string? confirm = null)
    {
        var folder = Path.Combine(_root, "mods", "notes");
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "mod.json"), """{"id":"notes","name":"Notes","version":"1.0.0","capabilities":["runtime"]}""");
        var c = confirm is null ? "" : $$""", "confirm": "{{confirm}}" """;
        File.WriteAllText(Path.Combine(folder, "extended.mod.json"), "{\n  \"schemaVersion\": 1,\n  \"enabled\": true,\n  \"settings\": { \"level\": { \"value\": 1, \"label\": \"Level\" } },\n" +
            $$"""  "groups": [ { "label": "Tools", "actions": [ { "id": "reset", "label": "Reset progress"{{c}} } ] } ]""" + "\n}\n");
        ShroudForgeUi.Load(folder);
        return folder;
    }

    // ---- the page

    private static CatalogMod Mod(string folder) =>
        new() { Engine = "shroudforge", Id = "notes", Name = "Notes", Folder = folder, State = ModRunState.Loaded, Active = true };

    private static ModManagerPage Page(CatalogMod mod) => new()
    {
        Source = () => new CatalogSnapshot
        {
            Version = 1, Mods = [mod], Engines = [new CatalogEngine { Kind = "shroudforge", Name = "ShroudForge", Count = 1 }],
        },
        Filter = new EngineFilter("shroudforge"), Selected = "shroudforge/notes",
    };

    private TimeSpan Frame(ModManagerPage page)
    {
        var t = Stopwatch.StartNew();
        _ui.Frame.Number++;
        page.Draw(_ui, new PageRect(0, 0, 1800, 900), 1);
        FakeUiTable.Pressed.Clear();
        return t.Elapsed;
    }

    /// <summary>Clicks the action, then draws frames until a new result line shows (it is sent on a worker).</summary>
    private string? ClickAction(ModManagerPage page, string button = "Reset progress")
    {
        FakeUiTable.Click = button;
        Frame(page);
        for (var i = 0; i < 300; i++)
        {
            lock (FakeUiTable.Texts) FakeUiTable.Texts.Clear();
            Frame(page);
            lock (FakeUiTable.Texts)
                if (FakeUiTable.Texts.FirstOrDefault(t => t.Contains("Reset progress", StringComparison.Ordinal) && t != "Reset progress" &&
                                                          !t.StartsWith("Confirm:", StringComparison.Ordinal)) is { } line && line != _lastLine)
                    return _lastLine = line;
            Thread.Sleep(10);
        }
        return null;
    }

    private static JsonElement Record(string path) => JsonDocument.Parse(File.ReadAllBytes(path)).RootElement;

    // ---- sent only when enqueued

    [Fact]
    public void A_kiln_start_sends_the_action_through_the_bridge_with_the_owner_epoch_never_shroudforges_folder()
    {
        State();
        var line = ClickAction(Page(Mod(ActionPackage())));
        Assert.NotNull(line);
        Assert.StartsWith("Requested", line);
        var sent = Assert.Single(Files(KilnActions));
        var r = Record(sent);
        Assert.Equal("action", r.GetProperty("kind").GetString());
        Assert.Equal(L, r.GetProperty("launchId").GetString());
        Assert.Equal(Plan, r.GetProperty("planHash").GetString());
        Assert.Equal(3, r.GetProperty("ownerEpoch").GetInt64());
        Assert.Equal("notes", r.GetProperty("modId").GetString());
        Assert.Equal("reset", r.GetProperty("action").GetString());
        Assert.False(r.TryGetProperty("value", out _));
        Assert.True(File.Exists(SfiBridgeFiles.ReceiptPath(Game, L)));
        Assert.Empty(Files(SfActions));
        lock (_hosted) Assert.Empty(_hosted);
    }

    [Fact]
    public void The_epoch_sent_is_the_one_sfi_json_holds_at_the_click()
    {
        State();
        var page = Page(Mod(ActionPackage()));
        Assert.StartsWith("Requested", ClickAction(page));
        Assert.Equal(3, Record(Assert.Single(Files(KilnActions))).GetProperty("ownerEpoch").GetInt64());
        foreach (var f in Files(KilnActions)) File.Delete(f);  // the runtime drained it
        State(o => { Notes(o)["ownerEpoch"] = 4; return o; });  // the package was reloaded
        FakeUiTable.Click = "Reset progress";  // the same line again: wait for the file, not for a new line
        Frame(page);
        Assert.True(SpinWait.SpinUntil(() => { Frame(page); return Files(KilnActions).Length == 1; }, 5000));
        Assert.Equal(4, Record(Assert.Single(Files(KilnActions))).GetProperty("ownerEpoch").GetInt64());
    }

    public static TheoryData<string> NotLive() => new()
    {
        "no-owner-epoch", "owner-epoch-zero", "owner-epoch-text", "runtime-stopped", "runtime-unknown", "not-listed", "listed-twice",
        "disabled", "phase-ended", "phase-refused", "no-packages",
    };

    /// <summary>Inactive: no live runtime would take the request, so nothing is written anywhere and the line says
    /// "not sent".</summary>
    [Theory]
    [MemberData(nameof(NotLive))]
    public void A_package_without_a_live_runtime_is_not_sent(string why)
    {
        State(o =>
        {
            var n = Notes(o);
            switch (why)
            {
                case "no-owner-epoch": n.Remove("ownerEpoch"); break;
                case "owner-epoch-zero": n["ownerEpoch"] = 0; break;
                case "owner-epoch-text": n["ownerEpoch"] = "3"; break;
                case "runtime-stopped": n["runtimeActive"] = false; n["lua"] = "stopped"; break;
                case "runtime-unknown": n["runtimeActive"] = null; n["lua"] = "stopped"; break;
                case "not-listed": n["id"] = "other"; break;
                case "listed-twice": o["packages"]!.AsArray().Add(n.DeepClone()); break;
                case "disabled": n["intent"] = "disabled"; break;
                case "phase-ended": o["phase"] = "ended"; o["incomplete"] = false; break;
                case "phase-refused": o["phase"] = "refused"; break;
                case "no-packages": o["packages"] = new JsonArray(); break;
            }
            return o;
        });
        var enqueued = 0;
        SfiBridge.Enqueue = (g, l, m, e, a, v) => { Interlocked.Increment(ref enqueued); return SfiBridgeFiles.EnqueueAction(g, l, m, e, a, v); };
        var line = ClickAction(Page(Mod(ActionPackage())));
        Assert.NotNull(line);
        Assert.Contains("not sent", line);
        Assert.Equal(0, Volatile.Read(ref enqueued));
        Assert.Empty(AnyBridgeFiles());
        Assert.Empty(Files(SfActions));
        lock (_hosted) Assert.Empty(_hosted);
    }

    [Fact]
    public void A_request_still_waiting_is_not_joined_by_another()
    {
        State();
        var page = Page(Mod(ActionPackage()));
        Assert.StartsWith("Requested", ClickAction(page));
        Assert.Contains("still waiting", ClickAction(page));
        Assert.Single(Files(KilnActions));
        foreach (var f in Files(KilnActions)) File.Delete(f);  // the runtime took it
        Assert.StartsWith("Requested", ClickAction(page));
        Assert.Single(Files(KilnActions));
    }

    /// <summary>Two clicks while the first request is being written: the waiting check and the write are one step.</summary>
    [Fact]
    public void Two_clicks_while_the_first_is_written_send_one()
    {
        State();
        using var writing = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var calls = 0;
        SfiBridge.Enqueue = (g, l, m, e, a, v) =>
        {
            if (Interlocked.Increment(ref calls) == 1)
            {
                writing.Set();
                release.Wait(TimeSpan.FromSeconds(5));
            }
            return SfiBridgeFiles.EnqueueAction(g, l, m, e, a, v);
        };
        try
        {
            var page = Page(Mod(ActionPackage()));
            FakeUiTable.Click = "Reset progress";
            Frame(page);
            Assert.True(writing.Wait(TimeSpan.FromSeconds(5)));
            FakeUiTable.Click = "Reset progress";
            Frame(page);
            Thread.Sleep(200);
            release.Set();
            Assert.True(SpinWait.SpinUntil(() =>
            {
                lock (FakeUiTable.Texts) FakeUiTable.Texts.Clear();
                Frame(page);
                lock (FakeUiTable.Texts) return FakeUiTable.Texts.Any(t => t.Contains("still waiting", StringComparison.Ordinal));
            }, 5000), "the second click did not report the request already waiting");
            Assert.Equal(1, Volatile.Read(ref calls));
            Assert.Single(Files(KilnActions));
        }
        finally { release.Set(); }
    }

    /// <summary>Full: the owner's bound of waiting requests is reached; nothing is written and the line says so.</summary>
    [Fact]
    public void A_full_queue_is_not_sent()
    {
        State();
        SfiBridge.Pending = (_, _, _) => 0;  // the bridge's own count (a first request) passes; the folder is full
        Directory.CreateDirectory(KilnActions);
        for (var i = 0; i < SfiBridgeFiles.PendingActionsPerOwner; i++)
            File.WriteAllText(Path.Combine(KilnActions, $"{Guid.NewGuid():D}.json"), """{"format":1,"kind":"action","modId":"notes"}""");
        var line = ClickAction(Page(Mod(ActionPackage())));
        Assert.NotNull(line);
        Assert.Contains("still waiting", line);
        Assert.Contains("nothing new was queued", line);
        Assert.Equal(SfiBridgeFiles.PendingActionsPerOwner, Files(KilnActions).Length);
        Assert.Empty(Files(SfActions));
    }

    /// <summary>Unavailable: the bridge's folder cannot be made (a file where it goes); nothing is written, "could not be
    /// sent".</summary>
    [Fact]
    public void An_unusable_bridge_folder_is_not_sent()
    {
        State();
        Directory.CreateDirectory(Path.Combine(_root, "kiln", "sfi"));
        File.WriteAllText(SfiBridgeFiles.UiRoot(Game), "not a folder");
        var line = ClickAction(Page(Mod(ActionPackage())));
        Assert.NotNull(line);
        Assert.Contains("could not be sent", line);
        Assert.Equal("not a folder", File.ReadAllText(SfiBridgeFiles.UiRoot(Game)));
        Assert.Empty(Files(SfActions));
        lock (_hosted) Assert.Empty(_hosted);
    }

    [Fact]
    public void An_action_the_package_does_not_declare_is_not_sent()
    {
        State();
        ActionPackage();
        var p = ShroudForgeUi.Get(Path.Combine(_root, "mods", "notes"), out _)!;
        var r = Task.Run(() => SfiBridge.Send(p, "wipe", "Wipe")).Result;
        Assert.NotNull(r);
        Assert.False(r.Value.Ok);
        Assert.Contains("declares no action wipe", r.Value.Text());
        Assert.Empty(AnyBridgeFiles());
    }

    [Fact]
    public void A_confirmed_action_needs_its_second_click_in_a_kiln_start_too()
    {
        State();
        var page = Page(Mod(ActionPackage(confirm: "Really?")));
        FakeUiTable.Click = "Reset progress";
        Frame(page);
        Frame(page);
        Thread.Sleep(100);
        Assert.Empty(AnyBridgeFiles());
        var line = ClickAction(page, "Confirm: Reset progress");
        Assert.StartsWith("Requested", line);
        Assert.Single(Files(KilnActions));
    }

    // ---- which start: the hosted path stays as it was

    public static TheoryData<string> HostedStarts() => new() { "no-file", "another-pid", "reused-pid", "hosted-backend", "installed-backend", "old-unreadable" };

    [Theory]
    [MemberData(nameof(HostedStarts))]
    public void Any_other_start_takes_the_hosted_path_unchanged(string start)
    {
        var processStart = DateTime.UtcNow.AddMinutes(-5);
        switch (start)
        {
            case "another-pid": State(o => { o["launch"]!["pid"] = Environment.ProcessId + 1; return o; }); break;
            case "reused-pid":
                // this pid, another process start
                ModCatalog.Sources = () => new CatalogSources { KilnRoot = Path.Combine(_root, "kiln"), ProcessStartUtc = processStart };
                State(o => { o["launch"]!["processStartMs"] = new DateTimeOffset(processStart).ToUnixTimeMilliseconds() - 60_000; return o; });
                File.SetLastWriteTimeUtc(StatePath, processStart.AddMinutes(-1));  // the earlier process ended before this one began
                break;
            case "hosted-backend":
                State(o =>
                {
                    o["launch"]!["backend"]!["selected"] = "hosted";
                    o["phase"] = "aside";
                    return o;
                });
                break;
            case "installed-backend": State(o => { o["launch"]!["backend"]!["selected"] = "installed"; return o; }); break;
            case "old-unreadable":
                ModCatalog.Sources = () => new CatalogSources { KilnRoot = Path.Combine(_root, "kiln"), ProcessStartUtc = processStart };
                Directory.CreateDirectory(Path.GetDirectoryName(StatePath)!);
                File.WriteAllText(StatePath, "{ broken");
                File.SetLastWriteTimeUtc(StatePath, processStart.AddHours(-1));
                break;
        }
        var line = ClickAction(Page(Mod(ActionPackage())));
        Assert.NotNull(line);
        Assert.StartsWith("Requested", line);
        lock (_hosted)
        {
            var (game, package, action) = Assert.Single(_hosted);
            Assert.Equal(Game, game);
            Assert.Equal("notes", package.Id);
            Assert.Equal("reset", action);
        }
        // ShroudForge's own request file, as ShroudForgePackages.QueueAction writes it
        var name = Path.GetFileName(Assert.Single(Files(SfActions)));
        Assert.Matches($"^notes-[0-9]+-[0-9a-f]{{32}}-{Environment.ProcessId}\\.json$", name);
        Assert.Matches("^\\{\"modId\":\"notes\",\"action\":\"reset\",\"createdAt\":[0-9]+\\}$", File.ReadAllText(Path.Combine(SfActions, name)));
        Assert.Empty(AnyBridgeFiles());
    }

    /// <summary>sfi.json written during this start but unreadable: which runtime runs is not known, so neither folder
    /// gets a request (one in ShroudForge's would run at a later hosted start).</summary>
    [Theory]
    [InlineData("{ broken")]
    [InlineData("[]")]
    [InlineData("{\"launch\":{\"pid\":PID}}")]
    [InlineData("{\"format\":1,\"launch\":{\"pid\":PID,\"processStartMs\":1,\"backend\":{\"selected\":\"kiln\"},\"launchId\":\"x\"},\"phase\":\"running\"}")]
    public void An_unreadable_sfi_json_of_this_start_sends_nothing(string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(StatePath)!);
        File.WriteAllText(StatePath, text.Replace("PID", Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        var line = ClickAction(Page(Mod(ActionPackage())));
        Assert.NotNull(line);
        Assert.Contains("could not be sent", line);
        Assert.Empty(AnyBridgeFiles());
        Assert.Empty(Files(SfActions));
        lock (_hosted) Assert.Empty(_hosted);
    }

    /// <summary>sfi.json names this pid with another process start but was written during this start: no earlier process
    /// with this pid can have written it (that one ended before this one began), so the start times disagree (a side's
    /// fallback start), not the process. Which runtime runs is not proved: neither folder gets a request.</summary>
    [Fact]
    public void This_pid_with_another_start_written_during_this_start_sends_nothing()
    {
        var processStart = DateTime.UtcNow.AddMinutes(-5);
        ModCatalog.Sources = () => new CatalogSources { KilnRoot = Path.Combine(_root, "kiln"), ProcessStartUtc = processStart };
        State(o => { o["launch"]!["processStartMs"] = new DateTimeOffset(processStart).ToUnixTimeMilliseconds() + 60_000; return o; });
        var line = ClickAction(Page(Mod(ActionPackage())));
        Assert.NotNull(line);
        Assert.Contains("could not be sent", line);
        Assert.Empty(AnyBridgeFiles());
        Assert.Empty(Files(SfActions));
        lock (_hosted) Assert.Empty(_hosted);
    }

    // ---- threads

    /// <summary>The render thread (here: the test's thread, which draws the frames) does no file IO for an action: the
    /// state read, the waiting count, the write and the cleanup run on workers, and a slow write never holds a frame.</summary>
    [Fact]
    public void No_file_io_on_the_render_thread()
    {
        State();
        var render = Environment.CurrentManagedThreadId;
        var io = new List<(string What, int Thread)>();
        SfiBridge.OnIo = what => { lock (io) io.Add((what, Environment.CurrentManagedThreadId)); };
        using var release = new ManualResetEventSlim();
        SfiBridge.Enqueue = (g, l, m, e, a, v) =>
        {
            release.Wait(TimeSpan.FromSeconds(10));
            return SfiBridgeFiles.EnqueueAction(g, l, m, e, a, v);
        };
        SfiBridge.Cleanup = (g, current) => { release.Wait(TimeSpan.FromSeconds(10)); return []; };
        try
        {
            var page = Page(Mod(ActionPackage()));
            FakeUiTable.Click = "Reset progress";
            var slowest = Frame(page);
            for (var i = 0; i < 20; i++)
            {
                slowest = TimeSpan.FromTicks(Math.Max(slowest.Ticks, Frame(page).Ticks));
                Thread.Sleep(10);
            }
            Assert.True(slowest < TimeSpan.FromSeconds(2), $"a frame took {slowest} while the write was held");
            lock (io) Assert.Contains(io, x => x.What == "enqueue");
        }
        finally { release.Set(); }
        Assert.True(SpinWait.SpinUntil(() => Files(KilnActions).Length == 1, 5000));
        SfiBridge.StartTask?.Wait(5000);
        lock (io)
        {
            Assert.Contains(io, x => x.What == "state");
            Assert.Contains(io, x => x.What == "cleanup");
            Assert.DoesNotContain(io, x => x.Thread == render);
        }
    }

    // ---- once per start: the cleanup of ended launches

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void The_cleanup_runs_once_per_start_on_a_worker_keeping_this_launch(bool kiln)
    {
        if (kiln) State();
        var calls = new List<(string Game, string? Current, int Thread)>();
        SfiBridge.Cleanup = (g, current) =>
        {
            lock (calls) calls.Add((g, current, Environment.CurrentManagedThreadId));
            return [];
        };
        var page = Page(Mod(ActionPackage()));
        Frame(page);
        Frame(page);
        SfiBridge.Start();
        Assert.True(SfiBridge.StartTask!.Wait(5000));
        Frame(page);
        lock (calls)
        {
            var c = Assert.Single(calls);
            Assert.Equal(Game, c.Game);
            Assert.Equal(kiln ? L : null, c.Current);
            Assert.NotEqual(Environment.CurrentManagedThreadId, c.Thread);
        }
    }

    /// <summary>Kiln.Ui's start (UiEngine.EnsureStarted) starts the cleanup right after ShroudForgeMenu.Start, so it runs in
    /// every start, not only one where a ShroudForge action is drawn; and Start, however often and from whichever threads it
    /// is called, prunes once, on a worker. (EnsureStarted itself is not run here: it starts the whole UI, its timers and
    /// texts, for the rest of the test run; its IL says what it calls.)</summary>
    [Fact]
    public void The_start_path_prunes_ended_launches_once_without_any_action_drawn()
    {
        var ensure = typeof(UiEngine).GetMethod(nameof(UiEngine.EnsureStarted), BindingFlags.Public | BindingFlags.Static)!;
        var il = ensure.GetMethodBody()!.GetILAsByteArray()!;
        var calls = new List<MethodBase>();
        for (var i = 0; i + 4 < il.Length; i++)
        {
            if (il[i] is not (0x28 or 0x6F)) continue;  // call, callvirt
            var token = BitConverter.ToInt32(il, i + 1);
            if ((token >>> 24) is not (0x06 or 0x0A or 0x2B)) continue;
            try { calls.Add(ensure.Module.ResolveMethod(token)!); }
            catch (Exception e) when (e is ArgumentException or BadImageFormatException) { continue; }
            i += 4;
        }
        var menu = calls.FindIndex(m => m.DeclaringType == typeof(ShroudForgeMenu) && m.Name == nameof(ShroudForgeMenu.Start));
        Assert.True(menu >= 0, "EnsureStarted no longer calls ShroudForgeMenu.Start");
        Assert.True(menu + 1 < calls.Count && calls[menu + 1].DeclaringType == typeof(SfiBridge) && calls[menu + 1].Name == nameof(SfiBridge.Start),
            "EnsureStarted does not call SfiBridge.Start right after ShroudForgeMenu.Start");

        // no page, no action: Start alone, from several threads at once
        State();
        var cleanups = new List<(string Game, string? Current, int Thread)>();
        SfiBridge.Cleanup = (g, current) =>
        {
            lock (cleanups) cleanups.Add((g, current, Environment.CurrentManagedThreadId));
            return [];
        };
        using var go = new ManualResetEventSlim();
        var callers = Enumerable.Range(0, 8).Select(_ => new Thread(() => { go.Wait(); SfiBridge.Start(); })).ToList();
        callers.ForEach(t => t.Start());
        go.Set();
        callers.ForEach(t => t.Join());
        SfiBridge.Start();
        Assert.True(SfiBridge.StartTask!.Wait(5000));
        lock (cleanups)
        {
            var c = Assert.Single(cleanups);
            Assert.Equal(Game, c.Game);
            Assert.Equal(L, c.Current);
            Assert.DoesNotContain(callers, t => t.ManagedThreadId == c.Thread);
            Assert.NotEqual(Environment.CurrentManagedThreadId, c.Thread);
        }
    }

    [Fact]
    public void The_real_cleanup_keeps_this_launch_and_removes_an_ended_one()
    {
        State();
        // this launch's folder (no receipt yet: kept anyway) and one an ended start left 30 days ago
        Directory.CreateDirectory(SfiBridgeFiles.ActionsDir(Game, L));
        const string old = "00000000000000a1";
        Directory.CreateDirectory(SfiBridgeFiles.ActionsDir(Game, old));
        File.WriteAllBytes(SfiBridgeFiles.ReceiptPath(Game, old),
            SfiBridgeFiles.ReceiptBytes(new SfiLaunch(old, Plan, 1, 1), 1));
        var at = DateTime.UtcNow.AddDays(-30);
        foreach (var e in Directory.EnumerateFileSystemEntries(SfiBridgeFiles.LaunchDir(Game, old), "*", SearchOption.AllDirectories).Append(SfiBridgeFiles.LaunchDir(Game, old)))
            if (Directory.Exists(e)) Directory.SetLastWriteTimeUtc(e, at); else File.SetLastWriteTimeUtc(e, at);
        SfiBridgeFiles.Liveness = (_, _) => SfiLiveness.Ended;
        try
        {
            SfiBridge.Start();
            Assert.True(SfiBridge.StartTask!.Wait(5000));
        }
        finally { SfiBridgeFiles.Liveness = SfiBridgeFiles.ProcessLiveness; }
        Assert.True(Directory.Exists(SfiBridgeFiles.LaunchDir(Game, L)));
        Assert.False(Directory.Exists(SfiBridgeFiles.LaunchDir(Game, old)));
    }

    // ---- the reader

    [Fact]
    public void Every_shared_state_fixture_reads_and_another_starts_file_is_hosted()
    {
        var dir = Path.Combine(SfiFixtures.Root(), "state");
        var files = Directory.EnumerateFiles(dir, "*.json", SearchOption.AllDirectories)
            .Where(f => !f.EndsWith("cases.json", StringComparison.Ordinal)).Order(StringComparer.Ordinal).ToList();
        Assert.NotEmpty(files);
        var epochs = 0;
        foreach (var f in files)
        {
            JsonNode? node;
            try { node = JsonNode.Parse(File.ReadAllText(f)); }
            catch (JsonException) { continue; }  // an invalid fixture may not be JSON at all
            if (node is not JsonObject o || o["launch"] is not JsonObject) continue;
            using (var doc = JsonDocument.Parse(o.ToJsonString()))
            {
                var pid = doc.RootElement.GetProperty("launch").TryGetProperty("pid", out var p) && p.TryGetInt64(out var v) ? v : -1;
                // the fixtures' process is never this one
                Assert.True(pid == Environment.ProcessId || SfiBridge.Parse(doc.RootElement, Environment.ProcessId, DateTime.MinValue).Mode == SfiMode.Hosted, f);
            }
            if (!f.Contains($"{Path.DirectorySeparatorChar}valid{Path.DirectorySeparatorChar}", StringComparison.Ordinal)) continue;
            o["launch"]!["pid"] = Environment.ProcessId;
            using var mine = JsonDocument.Parse(o.ToJsonString());
            var read = SfiBridge.Parse(mine.RootElement, Environment.ProcessId, DateTime.MinValue);
            var kiln = o["launch"]!["backend"]!["selected"]!.GetValue<string>() == "kiln";
            Assert.True(read.Mode == (kiln ? SfiMode.Kiln : SfiMode.Hosted), $"{f}: {read.Mode} {read.Why}");
            if (read.Claim is not { } claim) continue;
            // a fixture with the ownerEpoch member reads it for a live package, and never for another
            foreach (var pkg in o["packages"]!.AsArray().OfType<JsonObject>())
                if (pkg["ownerEpoch"] is JsonValue e && e.TryGetValue<long>(out var epoch))
                {
                    epochs++;
                    var live = claim.Live && pkg["runtimeActive"] is JsonValue ra && ra.TryGetValue<bool>(out var b) && b && pkg["intent"]!.GetValue<string>() == "selected";
                    Assert.Equal(live ? epoch : null, claim.EpochOf(pkg["id"]!.GetValue<string>()));
                }
                else Assert.Null(claim.EpochOf(pkg["id"]!.GetValue<string>()));
        }
        TestContext.Current.SendDiagnosticMessage($"state fixtures: {files.Count}, packages with ownerEpoch: {epochs}");
    }

    [Fact]
    public void The_reader_reads_this_starts_kiln_claim()
    {
        using var doc = JsonDocument.Parse(KilnState().ToJsonString());
        var start = DateTimeOffset.FromUnixTimeMilliseconds(1763520000000).UtcDateTime;
        var read = SfiBridge.Parse(doc.RootElement, Environment.ProcessId, start.AddMilliseconds(1500));
        Assert.Equal(SfiMode.Kiln, read.Mode);
        Assert.Equal(new SfiLaunch(L, Plan, Environment.ProcessId, 1763520000000), read.Claim!.Launch);
        Assert.Equal(3, read.Claim.EpochOf("notes"));
        Assert.Null(read.Claim.EpochOf("ammo-slots"));  // skipped: no live runtime
        // this pid with another start: a reused pid, or one of the two sides' start fallbacks; the file's time decides (ReadClaim)
        Assert.Equal(SfiMode.Unknown, SfiBridge.Parse(doc.RootElement, Environment.ProcessId, start.AddSeconds(5)).Mode);
        Assert.Equal(SfiMode.Hosted, SfiBridge.Parse(doc.RootElement, Environment.ProcessId + 1, DateTime.MinValue).Mode);
    }

    [Theory]
    [InlineData(null, false)]
    [InlineData("", false)]
    [InlineData("relative/game", false)]
    [InlineData("NUL", false)]
    [InlineData("MISSING", false)]
    [InlineData("ROOT", true)]
    public void The_game_folder_is_checked_before_any_write(string? game, bool usable)
    {
        var path = game switch
        {
            "NUL" => Path.Combine(_root, "a\0b"), "MISSING" => Path.Combine(_root, "missing"), "ROOT" => _root, _ => game,
        };
        Assert.Equal(usable, SfiBridge.UsableGameDir(path));
    }
}

/// <summary>The notice reader takes regular files only and never reads through a link.</summary>
[Collection("SfiBridgeFiles")]
public sealed class SfiNoticeFilesTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-sfi-notices-" + Guid.NewGuid().ToString("N"));
    private string ConfigDir => Path.Combine(_root, "config");
    private string Notices => SfiBridgeFiles.NoticesDir(ConfigDir);

    public SfiNoticeFilesTests()
    {
        Directory.CreateDirectory(Notices);
        SfiBridgeFiles.ResetForTests();
        SfiBridgeFiles.Log = (_, _) => { };
    }

    public void Dispose()
    {
        SfiBridgeFiles.ResetForTests();
        try { Directory.Delete(_root, true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static byte[] Wire(string name) => File.ReadAllBytes(Path.Combine(SfiFixtures.Root(), "ui", "wire", name));

    private static string FileName(byte[] notice)
    {
        using var d = JsonDocument.Parse(notice);
        return Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(d.RootElement.GetProperty("key").GetString()!))) + ".json";
    }

    private static IReadOnlyList<SfiNotice> ReadWithin(string configDir, out int skipped, Action? unblock = null)
    {
        var t = Task.Run(() => (SfiBridgeFiles.ReadNotices(configDir, out var s), s));
        if (!t.Wait(TimeSpan.FromSeconds(10)))
        {
            unblock?.Invoke();  // let a reader stuck on the FIFO (the bug) go
            Assert.Fail("ReadNotices blocked");
        }
        skipped = t.Result.s;
        return t.Result.Item1;
    }

    [Fact]
    public void A_fifo_named_like_a_notice_is_left_out_without_blocking()
    {
        if (OperatingSystem.IsWindows()) Assert.Skip("skipped: Windows folders hold no FIFO");
        var good = Wire("notice-https-action.json");
        File.WriteAllBytes(Path.Combine(Notices, FileName(good)), good);
        var fifo = Path.Combine(Notices, FileName(Wire("notice.json")));
        using (var mk = Process.Start(new ProcessStartInfo("mkfifo", [fifo]) { UseShellExecute = false })!)
        {
            Assert.True(mk.WaitForExit(10_000));
            Assert.Equal(0, mk.ExitCode);
        }
        // opening the FIFO for writing waits for its reader: only done when a reader is stuck on it
        var list = ReadWithin(ConfigDir, out var skipped, () => Task.Run(() =>
        {
            try { using var w = new FileStream(fifo, FileMode.Open, FileAccess.Write, FileShare.ReadWrite); }
            catch (IOException) { }
        }).Wait(TimeSpan.FromSeconds(5)));
        Assert.Single(list);
        Assert.Equal(1, skipped);
    }

    [Fact]
    public void A_notice_file_that_is_a_link_is_not_read_through()
    {
        var elsewhere = Path.Combine(_root, "elsewhere");
        Directory.CreateDirectory(elsewhere);
        var notice = Wire("notice.json");
        var target = Path.Combine(elsewhere, "real.json");
        File.WriteAllBytes(target, notice);
        if (!SfiFixtures.TryLink(Path.Combine(Notices, FileName(notice)), target, folder: false))
            Assert.Skip("skipped: this system does not let a test make a file link");
        var good = Wire("notice-https-action.json");
        File.WriteAllBytes(Path.Combine(Notices, FileName(good)), good);
        var list = ReadWithin(ConfigDir, out var skipped);
        Assert.Equal("https", Assert.Single(list).ActionUrl![..5]);
        Assert.Equal(1, skipped);
    }

    [Theory]
    [InlineData("notifications")]
    [InlineData("sfi")]
    public void A_notices_folder_reached_through_a_link_is_not_read(string linked)
    {
        var elsewhere = Path.Combine(_root, "elsewhere");
        var real = linked == "sfi" ? Path.Combine(elsewhere, "notifications") : elsewhere;
        Directory.CreateDirectory(real);
        var notice = Wire("notice.json");
        File.WriteAllBytes(Path.Combine(real, FileName(notice)), notice);
        var link = linked == "sfi" ? Path.Combine(ConfigDir, "sfi") : Notices;
        Directory.Delete(link, true);
        if (!SfiFixtures.TryLink(link, elsewhere, folder: true)) Assert.Skip("skipped: this system does not let a test make a folder link");
        Assert.Empty(ReadWithin(ConfigDir, out _));
    }

    [Fact]
    public void An_ordinary_notice_still_reads()
    {
        var notice = Wire("notice.json");
        File.WriteAllBytes(Path.Combine(Notices, FileName(notice)), notice);
        Assert.Single(ReadWithin(ConfigDir, out var skipped));
        Assert.Equal(0, skipped);
    }
}
