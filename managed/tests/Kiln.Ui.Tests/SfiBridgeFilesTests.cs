// SPDX-License-Identifier: MIT
//
// The UI bridge's files (SfiBridgeFiles.cs) against the cases kiln-sfi's runtime transport shares
// (schema/fixtures/sfi/ui/: wire/ bytes, bounds.json, drain/cases.json). The drain here is a model of the runtime's
// reading rules (drain/cases.json "rules"), so the producer's files are checked against the consumer the fixtures
// describe; kiln-sfi tests/runtime_ui_transport.rs runs the same cases on the real runtime.
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.SfiBridgeFilesTests;

[Collection("SfiBridgeFiles")]  // the bridge's hooks (Log, Liveness, KillPoint, NowMs) are process-wide
public sealed class SfiBridgeFilesTests : IDisposable
{
    private const string L = "3f2b8c1e5a7d4e2f";
    private static readonly string P = new('a', 64);
    private readonly string _root = Path.Combine(Path.GetTempPath(), "kiln-sfi-bridge-" + Guid.NewGuid().ToString("N"));
    private readonly List<string> _log = [];
    private string Game => Path.Combine(_root, "game");
    private string ConfigDir => Path.Combine(Game, "kiln", "config");
    private static readonly SfiLaunch Launch = new(L, new string('a', 64), 4242, 1763520000000);

    public SfiBridgeFilesTests()
    {
        Directory.CreateDirectory(Game);
        SfiBridgeFiles.ResetForTests();
        SfiBridgeFiles.Log = (level, text) => { lock (_log) _log.Add($"{level}: {text}"); };
    }

    public void Dispose()
    {
        SfiBridgeFiles.ResetForTests();
        try
        {
            if (!OperatingSystem.IsWindows())
                foreach (var d in Directory.EnumerateDirectories(_root, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = 0 }))
                    File.SetUnixFileMode(d, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            Directory.Delete(_root, true);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    // above the test assembly first (a copied build on a runner), then above this source file
    private static string Fixtures([CallerFilePath] string self = "")
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Path.GetDirectoryName(self) ?? "" })
            for (var d = new DirectoryInfo(start); d != null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "schema", "fixtures", "sfi", "ui", "bounds.json")))
                    return Path.Combine(d.FullName, "schema", "fixtures", "sfi", "ui");
        throw new InvalidOperationException("schema/fixtures/sfi/ui not found above the test assembly or its source");
    }

    private static byte[] Wire(string name) => File.ReadAllBytes(Path.Combine(Fixtures(), "wire", name));

    private string Actions => SfiBridgeFiles.ActionsDir(Game, L);
    private string Staging => SfiBridgeFiles.StagingDir(Game, L);
    private static string[] Names(string dir) =>
        Directory.Exists(dir) ? Directory.EnumerateFileSystemEntries(dir).Select(p => Path.GetFileName(p)).Order(StringComparer.Ordinal).ToArray() : [];

    // ---- bounds and bytes

    [Fact]
    public void The_bounds_are_the_shared_file()
    {
        using var b = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(Fixtures(), "bounds.json")));
        var r = b.RootElement;
        Assert.Equal(r.GetProperty("recordBytes").GetInt32(), SfiBridgeFiles.RecordBytes);
        Assert.Equal(r.GetProperty("pendingActionsPerOwner").GetInt32(), SfiBridgeFiles.PendingActionsPerOwner);
        Assert.Equal(r.GetProperty("noticeKeysPerProfile").GetInt32(), SfiBridgeFiles.NoticeKeysPerProfile);
        Assert.Equal(r.GetProperty("entriesPerTurn").GetInt32(), SfiBridgeFiles.EntriesPerTurn);
        Assert.Equal(r.GetProperty("receiptBytes").GetInt32(), SfiBridgeFiles.MaxReceiptBytes);
        Assert.Equal(r.GetProperty("keepEndedLaunchDays").GetInt32(), SfiBridgeFiles.KeepEndedLaunchDays);
    }

    public static TheoryData<string> WireFiles()
    {
        var d = new TheoryData<string>();
        foreach (var f in Directory.EnumerateFiles(Path.Combine(Fixtures(), "wire"), "*.json").Order(StringComparer.Ordinal)) d.Add(Path.GetFileName(f));
        return d;
    }

    private static SfiActionRecord ActionOf(JsonElement r) => new(
        r.GetProperty("requestId").GetString()!, r.GetProperty("sequence").GetInt64(), r.GetProperty("launchId").GetString()!,
        r.GetProperty("planHash").GetString()!, r.GetProperty("ownerEpoch").GetInt64(), r.GetProperty("modId").GetString()!,
        r.GetProperty("action").GetString()!, r.TryGetProperty("value", out var v) ? v.GetString() : null, r.GetProperty("createdMs").GetInt64());

    [Theory]
    [MemberData(nameof(WireFiles))]
    public void Every_wire_file_is_what_the_bridge_writes_or_reads(string name)
    {
        var bytes = Wire(name);
        using var doc = JsonDocument.Parse(bytes);
        var r = doc.RootElement;
        switch (r.GetProperty("kind").GetString())
        {
            case "action":
                Assert.Equal(bytes, SfiBridgeFiles.ActionBytes(ActionOf(r)));
                break;
            case "launch":
                var l = new SfiLaunch(r.GetProperty("launchId").GetString()!, r.GetProperty("planHash").GetString()!,
                    r.GetProperty("pid").GetInt32(), r.GetProperty("processStartMs").GetInt64());
                Assert.Equal(bytes, SfiBridgeFiles.ReceiptBytes(l, r.GetProperty("createdMs").GetInt64()));
                break;
            case "notice":
                var dir = SfiBridgeFiles.NoticesDir(ConfigDir);
                Directory.CreateDirectory(dir);
                File.WriteAllBytes(Path.Combine(dir, r.GetProperty("keySha256").GetString() + ".json"), bytes);
                var read = SfiBridgeFiles.ReadNotices(ConfigDir, out var skipped);
                Assert.Equal(0, skipped);
                var n = Assert.Single(read);
                var p = r.GetProperty("notice");
                Assert.Equal((r.GetProperty("key").GetString(), p.GetProperty("id").GetString(), p.GetProperty("modId").GetString(),
                        p.GetProperty("title").GetString(), p.GetProperty("message").GetString(), p.GetProperty("level").GetString(),
                        p.GetProperty("actionUrl").ValueKind == JsonValueKind.Null ? null : p.GetProperty("actionUrl").GetString(),
                        p.GetProperty("updatedAt").GetInt64(), r.GetProperty("launchId").GetString(), r.GetProperty("ownerEpoch").GetInt64()),
                    (n.Key, n.Id, n.ModId, n.Title, n.Message, n.Level, n.ActionUrl, n.UpdatedAt, n.LaunchId, n.OwnerEpoch));
                break;
            default:
                Assert.Fail(name);
                break;
        }
    }

    // ---- writing an action

    [Fact]
    public void A_queued_action_is_one_whole_record_named_by_its_request_with_the_receipt_beside_it()
    {
        SfiBridgeFiles.NowMs = () => 1763520010000;
        var a = SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", "Block äß");
        var b = SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", null);
        Assert.True(a.Sent && b.Sent, a.Reason + " / " + b.Reason);
        Assert.Equal(new[] { a.RequestId + ".json", b.RequestId + ".json" }.Order(StringComparer.Ordinal), Names(Actions));
        Assert.Empty(Names(Staging));
        var ra = ReadRecord(Path.Combine(Actions, a.RequestId + ".json"));
        var rb = ReadRecord(Path.Combine(Actions, b.RequestId + ".json"));
        Assert.Equal(new SfiActionRecord(a.RequestId!, ra.Sequence, L, P, 3, "world-editor", "copy", "Block äß", 1763520010000), ra);
        Assert.Equal(ra.Sequence + 1, rb.Sequence);
        Assert.Null(rb.Value);
        Assert.Matches("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", a.RequestId);
        // the receipt: the launch's identity, the wire form
        Assert.Equal(SfiBridgeFiles.ReceiptBytes(Launch, 1763520010000), File.ReadAllBytes(SfiBridgeFiles.ReceiptPath(Game, L)));
        Assert.Equal(new[] { "actions", "launch.json", "staging" }, Names(SfiBridgeFiles.LaunchDir(Game, L)));
    }

    private static SfiActionRecord ReadRecord(string path)
    {
        var bytes = File.ReadAllBytes(path);
        using var doc = JsonDocument.Parse(bytes);
        var r = ActionOf(doc.RootElement);
        Assert.Equal(bytes, SfiBridgeFiles.ActionBytes(r));  // canonical: what the runtime's fixtures expect
        return r;
    }

    public static TheoryData<string, string, long, string?, string> Refusals() => new()
    {
        { "", "copy", 3, null, "mod id" },
        { "a/b", "copy", 3, null, "mod id" },
        { "ä", "copy", 3, null, "mod id" },
        { new string('m', 81), "copy", 3, null, "mod id" },
        { "world-editor", "../copy", 3, null, "action id" },
        { "world-editor", "copy all", 3, null, "action id" },
        { "world-editor", "copy", 0, null, "epochs" },
        { "world-editor", "copy", 3, "lone <high surrogate> here", "well-formed" },
    };

    [Theory]
    [MemberData(nameof(Refusals))]
    public void A_refused_request_writes_nothing(string modId, string action, long epoch, string? value, string why)
    {
        var r = SfiBridgeFiles.EnqueueAction(Game, Launch, modId, epoch, action, value?.Replace("<high surrogate>", "\ud800"));
        Assert.Equal(SfiQueueStatus.Invalid, r.Status);
        Assert.False(r.Sent);
        Assert.Contains(why, r.Reason);
        Assert.False(Directory.Exists(SfiBridgeFiles.UiRoot(Game)));
    }

    [Fact]
    public void A_launch_without_its_identity_writes_nothing()
    {
        foreach (var l in new[]
                 {
                     Launch with { LaunchId = "3f2b8c1e-5a7d-4e2f-9b1c-0d4e6f8a2b3c" }, Launch with { LaunchId = "3F2B8C1E5A7D4E2F" },
                     Launch with { LaunchId = "../../shroudforge" }, Launch with { PlanHash = new string('A', 64) }, Launch with { Pid = 0 },
                 })
            Assert.Equal(SfiQueueStatus.Invalid, SfiBridgeFiles.EnqueueAction(Game, l, "world-editor", 3, "copy", null).Status);
        Assert.False(Directory.Exists(SfiBridgeFiles.UiRoot(Game)));
    }

    [Fact]
    public void A_record_over_the_bound_is_refused_and_one_at_the_bound_is_queued()
    {
        SfiBridgeFiles.NowMs = () => 1763520010000;
        var empty = SfiBridgeFiles.ActionBytes(new SfiActionRecord(Guid.NewGuid().ToString("D"), 0, L, P, 3, "world-editor", "copy", "", 1763520010000)).Length;
        // the sequence grows by a digit at most between here and the call: pad against the real record
        var fill = SfiBridgeFiles.RecordBytes - empty - 8;
        var over = SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", new string('v', SfiBridgeFiles.RecordBytes));
        Assert.Equal(SfiQueueStatus.TooLarge, over.Status);
        Assert.False(Directory.Exists(SfiBridgeFiles.UiRoot(Game)));
        var ok = SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", new string('v', fill));
        Assert.True(ok.Sent, ok.Reason);
        var len = new FileInfo(Path.Combine(Actions, ok.RequestId + ".json")).Length;
        Assert.InRange(len, SfiBridgeFiles.RecordBytes - 16, SfiBridgeFiles.RecordBytes);
        // exactly at the bound: still queued; one byte more: refused (multi-byte text counts in bytes)
        var at = SfiBridgeFiles.RecordBytes - (int)len + fill;
        var exact = SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", new string('v', at));
        Assert.True(exact.Sent, exact.Reason);
        Assert.Equal(SfiBridgeFiles.RecordBytes, new FileInfo(Path.Combine(Actions, exact.RequestId + ".json")).Length);
        Assert.Equal(SfiQueueStatus.TooLarge, SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", new string('v', at + 1)).Status);
        Assert.Equal(SfiQueueStatus.TooLarge, SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", new string('ä', at / 2 + 1)).Status);
    }

    [Fact]
    public void An_owner_with_a_full_queue_is_refused_and_others_are_not()
    {
        Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", null).Sent);
        var template = File.ReadAllBytes(Directory.EnumerateFiles(Actions).Single());
        for (var i = 1; i < SfiBridgeFiles.PendingActionsPerOwner; i++)
            File.WriteAllBytes(Path.Combine(Actions, $"w-{i}.json"), template);
        // entries for nobody do not count
        File.WriteAllText(Path.Combine(Actions, "junk.json"), "not JSON");
        Directory.CreateDirectory(Path.Combine(Actions, "folder"));
        Assert.Equal(SfiBridgeFiles.PendingActionsPerOwner, SfiBridgeFiles.PendingActions(Game, L, "world-editor"));
        var full = SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "paste", null);
        Assert.Equal(SfiQueueStatus.Full, full.Status);
        Assert.False(full.Sent);
        Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch, "other-mod", 1, "copy", null).Sent);
        File.Delete(Path.Combine(Actions, "w-1.json"));
        Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "paste", null).Sent);
        Assert.Equal(SfiQueueStatus.Full, SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "paste", null).Status);
        Assert.Empty(Names(Staging));
    }

    public static TheoryData<string> KillSteps() => new()
    {
        "receipt:half-written", "receipt:written", "receipt:before-rename", "receipt:after-rename",
        "action:half-written", "action:written", "action:before-rename", "action:after-rename",
    };

    [Theory]
    [MemberData(nameof(KillSteps))]
    public void A_write_killed_at_any_step_leaves_actions_whole_or_empty(string step)
    {
        SfiBridgeFiles.KillPoint = s => { if (s == step) throw new SfiKillPointException(s); };
        Assert.Throws<SfiKillPointException>(() => SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", "Block äß"));
        var actions = Names(Actions);
        var drain = new DrainModel(Actions, L, P) { Owners = { ["world-editor"] = 3 }, Registered = { "world-editor copy" } };
        drain.Turn();
        Assert.Equal(0, drain.Malformed);  // the runtime never met half a record
        if (step == "action:after-rename")
        {
            Assert.Single(actions);
            Assert.Equal(new[] { ("world-editor", "copy", (string?)"Block äß") }, drain.Calls);
        }
        else
        {
            Assert.Empty(actions);
            Assert.Empty(drain.Calls);
        }
        Assert.Equal(step.StartsWith("receipt:") && step != "receipt:after-rename" ? 0 : 1,
            File.Exists(SfiBridgeFiles.ReceiptPath(Game, L)) ? 1 : 0);
        // the next request goes through; what the kill left in staging/ stays out of actions/
        SfiBridgeFiles.KillPoint = null;
        var next = SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", "again");
        Assert.True(next.Sent, next.Reason);
        Assert.True(File.Exists(SfiBridgeFiles.ReceiptPath(Game, L)));
        Assert.All(Names(Staging), n => Assert.EndsWith(".tmp", n));
        Assert.DoesNotContain(Names(Actions), n => n.EndsWith(".tmp", StringComparison.Ordinal));
        drain.Turn();
        Assert.Equal(0, drain.Malformed);
        Assert.Contains(("world-editor", "copy", "again"), drain.Calls);
    }

    [Fact]
    public void A_drain_running_beside_the_writer_never_sees_a_partial_or_temporary_entry()
    {
        const int count = 400;
        var drain = new DrainModel(Actions, L, P) { Owners = { ["world-editor"] = 3 }, Registered = { "world-editor copy" } };
        var writer = Task.Run(() =>
        {
            for (var i = 0; i < count; i++)
                Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", $"v{i:D4}" + new string('x', 4096)).Sent);
        });
        var seen = new List<string>();
        while (true)
        {
            var finished = writer.IsCompleted;
            if (Directory.Exists(Actions))
            {
                foreach (var n in Names(Actions)) seen.Add(n);
                drain.Turn();
            }
            if (finished && Names(Actions).Length == 0) break;
        }
        writer.GetAwaiter().GetResult();
        Assert.Equal(0, drain.Malformed);
        // only an entry already run may come back unreadable (its removal pending while the writer counts it)
        Assert.True(drain.Unread.All(u => drain.CalledEntries.Contains(u[..u.IndexOf(':')])), string.Join("\n", drain.Unread));
        Assert.DoesNotContain(seen, n => !n.EndsWith(".json", StringComparison.Ordinal));
        Assert.Equal(Enumerable.Range(0, count).Select(i => $"v{i:D4}"), drain.Calls.Select(c => c.Value![..5]).Order(StringComparer.Ordinal));
    }

    [Fact]
    public void The_bridge_never_writes_through_a_link()
    {
        var elsewhere = Path.Combine(_root, "elsewhere");
        Directory.CreateDirectory(elsewhere);
        Directory.CreateDirectory(Path.Combine(Game, "kiln", "sfi"));
        if (!TryLink(SfiBridgeFiles.UiRoot(Game), elsewhere)) Assert.Skip("skipped: this system does not let a test make a folder link");
        var r = SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", null);
        Assert.Equal(SfiQueueStatus.Unavailable, r.Status);
        Assert.Contains("link", r.Reason);
        Assert.Empty(Directory.EnumerateFileSystemEntries(elsewhere));
    }

    // a symbolic link; on Windows without the right to make one, a junction (what a player's tools would make)
    private static bool TryLink(string link, string target)
    {
        try { Directory.CreateSymbolicLink(link, target); return true; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or PlatformNotSupportedException) { }
        if (!OperatingSystem.IsWindows()) return false;
        using var p = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe")
        {
            ArgumentList = { "/c", "mklink", "/J", link, target }, UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true,
        })!;
        p.WaitForExit();
        return p.ExitCode == 0 && Directory.Exists(link);
    }

    // ---- the drain cases (one copy with kiln-sfi's runtime)

    private static JsonElement DrainCases()
    {
        using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(Fixtures(), "drain", "cases.json")));
        return doc.RootElement.Clone();
    }

    public static TheoryData<string> DrainCaseNames()
    {
        var d = new TheoryData<string>();
        foreach (var c in DrainCases().GetProperty("cases").EnumerateArray()) d.Add(c.GetProperty("name").GetString()!);
        return d;
    }

    private static string CallText((string Mod, string Action, string? Value) c) => $"{c.Mod} {c.Action} {(c.Value is null ? "nil" : JsonSerializer.Serialize(c.Value))}";
    private static string[] CallTexts(JsonElement calls) =>
        calls.EnumerateArray().Select(c => CallText((c.GetProperty("modId").GetString()!, c.GetProperty("action").GetString()!,
            c.GetProperty("value").ValueKind == JsonValueKind.Null ? null : c.GetProperty("value").GetString()))).Order(StringComparer.Ordinal).ToArray();

    [Theory]
    [MemberData(nameof(DrainCaseNames))]
    public void The_shared_drain_cases_hold_on_real_files(string name)
    {
        var c = DrainCases().GetProperty("cases").EnumerateArray().Single(x => x.GetProperty("name").GetString() == name);
        using var launch = JsonDocument.Parse(Wire("launch.json"));
        var dir = Actions;
        Directory.CreateDirectory(dir);
        foreach (var e in c.GetProperty("entries").EnumerateArray()) MakeEntry(dir, e);
        var drain = new DrainModel(dir, launch.RootElement.GetProperty("launchId").GetString()!, launch.RootElement.GetProperty("planHash").GetString()!);
        foreach (var o in c.GetProperty("owners").EnumerateObject()) drain.Owners[o.Name] = o.Value.GetInt64();
        foreach (var r in c.GetProperty("registered").EnumerateArray()) drain.Registered.Add(r.GetString()!);
        if (c.TryGetProperty("failing", out var failing)) foreach (var f in failing.EnumerateArray()) drain.Failing.Add(f.GetString()!);
        var all = new List<string>();
        var turn = 0;
        foreach (var t in c.GetProperty("turns").EnumerateArray())
        {
            turn++;
            if (t.TryGetProperty("register", out var reg)) foreach (var r in reg.EnumerateArray()) drain.Registered.Add(r.GetString()!);
            if (t.TryGetProperty("deactivate", out var off)) foreach (var o in off.EnumerateArray()) drain.Owners.Remove(o.GetString()!);
            if (t.TryGetProperty("epochs", out var ep)) foreach (var o in ep.EnumerateObject()) drain.Owners[o.Name] = o.Value.GetInt64();
            drain.RemoveFails.Clear();
            if (t.TryGetProperty("removeFails", out var rf)) foreach (var f in rf.EnumerateArray()) drain.RemoveFails.Add(f.GetString()!);
            drain.Calls.Clear();
            // another program (an antivirus scan, a backup) holds these open without sharing for the whole turn
            var held = t.TryGetProperty("locked", out var lk)
                ? lk.EnumerateArray().Select(x => new FileStream(Path.Combine(dir, x.GetString()!), FileMode.Open, FileAccess.Read, FileShare.None)).ToList()
                : [];
            int examined;
            try { examined = drain.Turn(); }
            finally { foreach (var h in held) h.Dispose(); }
            Assert.True(examined <= SfiBridgeFiles.EntriesPerTurn, $"turn {turn}: {examined} entries");
            var calls = drain.Calls.Select(CallText).Order(StringComparer.Ordinal).ToArray();
            all.AddRange(calls);
            if (t.TryGetProperty("calls", out var expected)) Assert.Equal(CallTexts(expected), calls);
            if (t.TryGetProperty("remaining", out var rem))
                Assert.Equal(rem.EnumerateArray().Select(x => x.GetString()!).Order(StringComparer.Ordinal), Names(dir));
            else Assert.Equal(t.GetProperty("remainingCount").GetInt32(), Names(dir).Length);
        }
        if (c.TryGetProperty("allCalls", out var allCalls)) Assert.Equal(CallTexts(allCalls), all.Order(StringComparer.Ordinal));
    }

    private static void MakeEntry(string dir, JsonElement e)
    {
        var path = Path.Combine(dir, e.GetProperty("name").GetString()!);
        if (e.TryGetProperty("folder", out _)) { Directory.CreateDirectory(path); return; }
        if (e.TryGetProperty("text", out var text)) { File.WriteAllBytes(path, new UTF8Encoding(false).GetBytes(text.GetString()!)); return; }
        var bytes = Wire(e.GetProperty("wire").GetString()!);
        if (e.TryGetProperty("set", out _) || e.TryGetProperty("drop", out _))
        {
            var o = JsonNode.Parse(bytes)!.AsObject();
            if (e.TryGetProperty("set", out var set)) foreach (var p in set.EnumerateObject()) o[p.Name] = JsonNode.Parse(p.Value.GetRawText());
            if (e.TryGetProperty("drop", out var drop)) foreach (var d in drop.EnumerateArray()) o.Remove(d.GetString()!);
            bytes = Encoding.UTF8.GetBytes(o.ToJsonString(new JsonSerializerOptions { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
        }
        if (e.TryGetProperty("padTo", out var pad))
        {
            var padded = new byte[pad.GetInt32()];
            Array.Fill(padded, (byte)' ');
            bytes.CopyTo(padded, 0);
            bytes = padded;
        }
        File.WriteAllBytes(path, bytes);
    }

    [Fact]
    public void The_drain_takes_what_the_bridge_wrote()
    {
        Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "copy", "q\"b\\ \n ä 😀").Sent);
        Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "later", null).Sent);
        Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 2, "copy", "stale epoch").Sent);
        Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch with { PlanHash = new string('b', 64) }, "world-editor", 3, "copy", "other plan").Sent);
        var drain = new DrainModel(Actions, L, P) { Owners = { ["world-editor"] = 3 }, Registered = { "world-editor copy" } };
        drain.Turn();
        Assert.Equal(new[] { ("world-editor", "copy", (string?)"q\"b\\ \n ä 😀") }, drain.Calls);
        Assert.Equal(1, SfiBridgeFiles.PendingActions(Game, L, "world-editor"));  // "later" waits for its callback
        Assert.Equal(0, drain.Malformed);
    }

    // ---- notices

    private string WriteNotice(string wire, Func<JsonObject, JsonObject>? change = null, string? fileName = null, byte[]? raw = null)
    {
        var dir = SfiBridgeFiles.NoticesDir(ConfigDir);
        Directory.CreateDirectory(dir);
        var bytes = Wire(wire);
        var sha = JsonNode.Parse(bytes)!["keySha256"]!.GetValue<string>();
        if (change is not null) bytes = Encoding.UTF8.GetBytes(change(JsonNode.Parse(bytes)!.AsObject()).ToJsonString(new JsonSerializerOptions { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
        var path = Path.Combine(dir, (fileName ?? sha) + ".json");
        File.WriteAllBytes(path, raw ?? bytes);
        return path;
    }

    [Fact]
    public void Notices_of_earlier_launches_stay_visible_newest_first()
    {
        WriteNotice("notice.json", o => { o["launchId"] = "0000000000000001"; o["notice"]!["updatedAt"] = 10; return o; });
        WriteNotice("notice-https-action.json");
        var read = SfiBridgeFiles.ReadNotices(ConfigDir, out var skipped);
        Assert.Equal(0, skipped);
        Assert.Equal(new[] { "world-editor-update", "world-editor-saved" }, read.Select(n => n.Key));
        Assert.Equal("0000000000000001", read[1].LaunchId);
        Assert.Equal("https://example.invalid/changelog?a=1&b=2", read[0].ActionUrl);
        Assert.Empty(SfiBridgeFiles.ReadNotices(Path.Combine(_root, "no-config"), out _));
    }

    [Fact]
    public void The_later_of_two_colliding_keys_is_the_notice()
    {
        var a = WriteNotice("notice-collision-1.json");
        var b = WriteNotice("notice-collision-2.json");
        Assert.Equal(a, b);
        var n = Assert.Single(SfiBridgeFiles.ReadNotices(ConfigDir, out _));
        Assert.Equal(("a", "a.b-c", "a-b-c"), (n.ModId, n.Id, n.Key));
    }

    public static TheoryData<string> BadNotices() => new()
    {
        "wrong-file-name", "upper-case-file-name", "not-json-name", "truncated", "invalid-utf8", "empty", "level-warning", "level-debug",
        "http-url", "url-number", "title-121-bytes", "message-2001-bytes", "extra-member", "extra-notice-member", "duplicate-member",
        "missing-updatedAt", "format-2", "kind-action", "key-not-modId-id", "id-not-modId-dot", "epoch-zero", "launch-uuid",
        "updatedAt-fraction", "over-record-bound", "a-folder",
    };

    [Theory]
    [MemberData(nameof(BadNotices))]
    public void A_notice_the_runtime_could_not_have_written_is_left_out_and_left_alone(string bad)
    {
        WriteNotice("notice-https-action.json");  // one good notice beside it
        var whole = Wire("notice.json");
        var sha = JsonNode.Parse(whole)!["keySha256"]!.GetValue<string>();
        var path = bad switch
        {
            "wrong-file-name" => WriteNotice("notice.json", fileName: new string('0', 64)),
            "upper-case-file-name" => WriteNotice("notice.json", fileName: sha.ToUpperInvariant()),
            "not-json-name" => WriteNotice("notice.json", fileName: sha + ".tmp"),
            "truncated" => WriteNotice("notice.json", raw: whole[..(whole.Length / 2)]),
            "invalid-utf8" => WriteNotice("notice.json", raw: InvalidUtf8(whole)),
            "empty" => WriteNotice("notice.json", raw: Array.Empty<byte>()),
            "level-warning" => WriteNotice("notice.json", o => { o["notice"]!["level"] = "warning"; return o; }),
            "level-debug" => WriteNotice("notice.json", o => { o["notice"]!["level"] = "debug"; return o; }),
            "http-url" => WriteNotice("notice.json", o => { o["notice"]!["actionUrl"] = "http://example.invalid/"; return o; }),
            "url-number" => WriteNotice("notice.json", o => { o["notice"]!["actionUrl"] = 5; return o; }),
            "title-121-bytes" => WriteNotice("notice.json", o => { o["notice"]!["title"] = new string('ä', 60) + "t"; return o; }),
            "message-2001-bytes" => WriteNotice("notice.json", o => { o["notice"]!["message"] = new string('ä', 1000) + "m"; return o; }),
            "extra-member" => WriteNotice("notice.json", o => { o["read"] = true; return o; }),
            "extra-notice-member" => WriteNotice("notice.json", o => { o["notice"]!["banner"] = "x"; return o; }),
            "duplicate-member" => WriteNotice("notice.json", raw: Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(whole).Replace("\"ownerEpoch\":3", "\"ownerEpoch\":3,\"ownerEpoch\":4"))),
            "missing-updatedAt" => WriteNotice("notice.json", o => { o["notice"]!.AsObject().Remove("updatedAt"); return o; }),
            "format-2" => WriteNotice("notice.json", o => { o["format"] = 2; return o; }),
            "kind-action" => WriteNotice("notice.json", o => { o["kind"] = "action"; return o; }),
            "key-not-modId-id" => WriteNotice("notice.json", o => { o["notice"]!["id"] = "world-editor.other"; return o; }),
            "id-not-modId-dot" => WriteNotice("notice.json", o => { o["notice"]!["id"] = "saved"; return o; }),
            "epoch-zero" => WriteNotice("notice.json", o => { o["ownerEpoch"] = 0; return o; }),
            "launch-uuid" => WriteNotice("notice.json", o => { o["launchId"] = "3f2b8c1e-5a7d-4e2f-9b1c-0d4e6f8a2b3c"; return o; }),
            "updatedAt-fraction" => WriteNotice("notice.json", raw: Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(whole).Replace("\"updatedAt\":1763520011", "\"updatedAt\":1763520011.5"))),
            "over-record-bound" => WriteNotice("notice.json", raw: whole.Concat(Enumerable.Repeat((byte)' ', SfiBridgeFiles.RecordBytes)).ToArray()),
            "a-folder" => Directory.CreateDirectory(Path.Combine(SfiBridgeFiles.NoticesDir(ConfigDir), sha + ".json")).FullName,
            _ => throw new ArgumentException(bad),
        };
        var before = File.Exists(path) ? File.ReadAllBytes(path) : null;
        var read = SfiBridgeFiles.ReadNotices(ConfigDir, out var skipped);
        Assert.Equal(new[] { "world-editor-update" }, read.Select(n => n.Key));
        Assert.Equal(bad == "a-folder" ? 0 : 1, skipped);
        if (before is not null) Assert.Equal(before, File.ReadAllBytes(path));  // left as it was: the reader never writes
        else Assert.True(Directory.Exists(path));
    }

    // a byte that is not UTF-8 inside the message's text
    private static byte[] InvalidUtf8(byte[] whole)
    {
        var bytes = (byte[])whole.Clone();
        bytes[Encoding.UTF8.GetString(whole).IndexOf("Your", StringComparison.Ordinal)] = 0xff;
        return bytes;
    }

    [Fact]
    public void Byte_limits_are_bytes_not_characters()
    {
        WriteNotice("notice.json", o => { o["notice"]!["title"] = new string('ä', 60); o["notice"]!["message"] = new string('ä', 1000); return o; });
        var n = Assert.Single(SfiBridgeFiles.ReadNotices(ConfigDir, out _));
        Assert.Equal(120, Encoding.UTF8.GetByteCount(n.Title));
    }

    [Fact]
    public void At_most_the_profiles_notice_keys_are_read()
    {
        var dir = SfiBridgeFiles.NoticesDir(ConfigDir);
        Directory.CreateDirectory(dir);
        var template = JsonNode.Parse(Wire("notice.json"))!.AsObject();
        for (var i = 0; i <= SfiBridgeFiles.NoticeKeysPerProfile; i++)
        {
            var local = $"n{i}";
            var key = "world-editor-" + local;
            var sha = Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(key)));
            var o = template.DeepClone().AsObject();
            o["key"] = key;
            o["keySha256"] = sha;
            o["notice"]!["id"] = "world-editor." + local;
            File.WriteAllText(Path.Combine(dir, sha + ".json"), o.ToJsonString());
        }
        var read = SfiBridgeFiles.ReadNotices(ConfigDir, out var skipped);
        Assert.Equal(SfiBridgeFiles.NoticeKeysPerProfile, read.Count);
        Assert.Equal(1, skipped);
        Assert.Contains(_log, l => l.Contains("more than " + SfiBridgeFiles.NoticeKeysPerProfile));
    }

    [Fact]
    public void Kiln_Ui_has_no_way_to_publish_a_notice()
    {
        // publish is the runtime's synchronous write, so a failed write fails the same Lua call; nothing here may queue one
        var names = typeof(SfiBridgeFiles).GetMethods(System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Public |
                                                      System.Reflection.BindingFlags.NonPublic).Select(m => m.Name).ToArray();
        Assert.DoesNotContain(names, n => n.Contains("Notice", StringComparison.Ordinal) && !n.StartsWith("Read", StringComparison.Ordinal) && n != "NoticesDir");
        Assert.DoesNotContain(names, n => n.Contains("Publish", StringComparison.Ordinal) && n != "Publish");
    }

    // ---- cleanup

    private const long Day = 86_400_000L;
    private static readonly DateTime Now = new(2026, 11, 20, 12, 0, 0, DateTimeKind.Utc);

    private string MakeLaunch(string id, int pid, long start, double daysIdle, bool receipt = true, string? receiptText = null)
    {
        var dir = SfiBridgeFiles.LaunchDir(Game, id);
        Directory.CreateDirectory(Path.Combine(dir, "actions"));
        Directory.CreateDirectory(Path.Combine(dir, "staging"));
        File.WriteAllBytes(Path.Combine(dir, "actions", Guid.NewGuid().ToString("D") + ".json"), Wire("action-with-value.json"));
        File.WriteAllText(Path.Combine(dir, "staging", "left.tmp"), "{\"format\":1,");
        if (receiptText is not null) File.WriteAllText(Path.Combine(dir, "launch.json"), receiptText);
        else if (receipt) File.WriteAllBytes(Path.Combine(dir, "launch.json"), SfiBridgeFiles.ReceiptBytes(Launch with { LaunchId = id, Pid = pid, ProcessStartMs = start }, 0));
        Age(dir, daysIdle);
        return dir;
    }

    private static void Age(string dir, double days)
    {
        var t = Now.AddDays(-days);
        foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)) File.SetLastWriteTimeUtc(f, t);
        foreach (var d in Directory.EnumerateDirectories(dir, "*", SearchOption.AllDirectories).Append(dir)) Directory.SetLastWriteTimeUtc(d, t);
    }

    private string[] Sentinels()
    {
        var sf = Path.Combine(Game, "shroudforge", "ui", "actions");
        Directory.CreateDirectory(sf);
        File.WriteAllText(Path.Combine(sf, "keep.json"), "{}");
        Age(Path.Combine(Game, "shroudforge"), 30);
        WriteNotice("notice.json");
        Age(SfiBridgeFiles.NoticesDir(ConfigDir), 30);
        return [Path.Combine(sf, "keep.json"), Directory.EnumerateFiles(SfiBridgeFiles.NoticesDir(ConfigDir)).Single()];
    }

    [Fact]
    public void Cleanup_removes_only_long_ended_launches_with_a_receipt()
    {
        var sentinels = Sentinels();
        SfiBridgeFiles.NowMs = () => new DateTimeOffset(Now).ToUnixTimeMilliseconds();
        // pid 100: ended; 200: running with start 2000; 300: cannot be told
        SfiBridgeFiles.Liveness = (pid, start) => pid switch
        {
            200 when start == 2000 => SfiLiveness.Alive,
            300 => SfiLiveness.Unknown,
            _ => SfiLiveness.Ended,
        };
        MakeLaunch("00000000000000a1", 100, 1000, 8);                    // ended 8 days ago: removed
        MakeLaunch("00000000000000a2", 100, 1000, 6.9);                  // ended, kept for diagnostics
        MakeLaunch("00000000000000a3", 200, 2000, 30);                   // still running
        MakeLaunch("00000000000000a4", 200, 9999, 30);                   // the pid was reused: ended
        MakeLaunch("00000000000000a5", 300, 1000, 30);                   // unknown: kept
        MakeLaunch("00000000000000a6", 100, 1000, 30, receipt: false);   // no receipt: kept
        MakeLaunch("00000000000000a7", 100, 1000, 30, receiptText: "{\"format\":1,\"kind\":\"launch\"");  // partial: kept
        MakeLaunch("00000000000000a8", 100, 1000, 30, receiptText: Encoding.UTF8.GetString(
            SfiBridgeFiles.ReceiptBytes(Launch with { LaunchId = "00000000000000ff", Pid = 100, ProcessStartMs = 1000 }, 0)));  // another folder's
        MakeLaunch(L, 100, 1000, 30);                                    // this launch, whatever its receipt says
        MakeLaunch("not-a-launch", 100, 1000, 30);
        var stray = Path.Combine(SfiBridgeFiles.UiRoot(Game), "0000000000000aaa.txt");
        File.WriteAllText(stray, "x");
        var outcomes = SfiBridgeFiles.Cleanup(Game, L).ToDictionary(o => o.Name, o => (o.Removed, o.Why));
        Assert.Equal((true, "ended"), outcomes["00000000000000a1"]);
        Assert.Equal((false, "ended, kept for diagnostics"), outcomes["00000000000000a2"]);
        Assert.Equal((false, "running"), outcomes["00000000000000a3"]);
        Assert.Equal((true, "ended"), outcomes["00000000000000a4"]);
        Assert.Equal((false, "liveness unknown"), outcomes["00000000000000a5"]);
        Assert.Equal((false, "no receipt"), outcomes["00000000000000a6"]);
        Assert.Equal((false, "unreadable receipt"), outcomes["00000000000000a7"]);
        Assert.Equal((false, "unreadable receipt"), outcomes["00000000000000a8"]);
        Assert.Equal((false, "this launch"), outcomes[L]);
        Assert.Equal((false, "not a launch folder"), outcomes["not-a-launch"]);
        Assert.Equal((false, "not a launch folder"), outcomes["0000000000000aaa.txt"]);
        Assert.False(Directory.Exists(SfiBridgeFiles.LaunchDir(Game, "00000000000000a1")));
        Assert.False(Directory.Exists(SfiBridgeFiles.LaunchDir(Game, "00000000000000a4")));
        foreach (var kept in new[] { "00000000000000a2", "00000000000000a3", "00000000000000a5", "00000000000000a6", "00000000000000a7", "00000000000000a8", L, "not-a-launch" })
            Assert.Equal(3, Directory.EnumerateFileSystemEntries(SfiBridgeFiles.LaunchDir(Game, kept), "*", SearchOption.AllDirectories)
                .Count(p => !Directory.Exists(p)) + (kept is "00000000000000a6" ? 1 : 0));
        Assert.True(File.Exists(stray));
        Assert.All(sentinels, s => Assert.True(File.Exists(s), s));
    }

    [Fact]
    public void Cleanup_never_follows_a_link()
    {
        var sentinels = Sentinels();
        SfiBridgeFiles.NowMs = () => new DateTimeOffset(Now).ToUnixTimeMilliseconds();
        SfiBridgeFiles.Liveness = (_, _) => SfiLiveness.Ended;
        var sf = Path.Combine(Game, "shroudforge");
        // a launch folder that is a link to <game>/shroudforge, and an ended launch holding a link to it
        Directory.CreateDirectory(SfiBridgeFiles.UiRoot(Game));
        if (!TryLink(SfiBridgeFiles.LaunchDir(Game, "00000000000000b1"), sf)) Assert.Skip("skipped: this system does not let a test make a folder link");
        var holder = MakeLaunch("00000000000000b2", 100, 1000, 30);
        Assert.True(TryLink(Path.Combine(holder, "actions", "sf"), sf));
        var outcomes = SfiBridgeFiles.Cleanup(Game, L).ToDictionary(o => o.Name, o => (o.Removed, o.Why));
        Assert.Equal((false, "a link"), outcomes["00000000000000b1"]);
        Assert.Equal((false, "holds a link"), outcomes["00000000000000b2"]);
        Assert.All(sentinels, s => Assert.True(File.Exists(s), s));
        Assert.True(Directory.Exists(Path.Combine(holder, "actions", "sf")));

        // the bridge's own folder is a link: nothing at all
        var ui = SfiBridgeFiles.UiRoot(Game);
        Directory.Move(ui, Path.Combine(_root, "real-ui"));
        Assert.True(TryLink(ui, Path.Combine(_root, "real-ui")));
        MakeLaunch("00000000000000b3", 100, 1000, 30);
        Assert.Equal(new[] { new SfiCleanupOutcome("", false, "a link on the path") }, SfiBridgeFiles.Cleanup(Game, L));
        Assert.True(Directory.Exists(Path.Combine(_root, "real-ui", "00000000000000b3")));
    }

    [Fact]
    public void A_failed_removal_logs_and_keeps_the_ended_launch_out_of_the_live_one()
    {
        if (OperatingSystem.IsWindows())
        {
            Assert.Skip("skipped: a read-only folder does not stop deletion on Windows (covered by the Unix runs)");
            return;
        }
        SfiBridgeFiles.NowMs = () => new DateTimeOffset(Now).ToUnixTimeMilliseconds();
        SfiBridgeFiles.Liveness = (_, _) => SfiLiveness.Ended;
        var old = MakeLaunch("00000000000000c1", 100, 1000, 30);
        var actions = Path.Combine(old, "actions");
        File.SetUnixFileMode(actions, UnixFileMode.UserRead | UnixFileMode.UserExecute);
        var outcome = Assert.Single(SfiBridgeFiles.Cleanup(Game, L));
        File.SetUnixFileMode(actions, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        Assert.False(outcome.Removed);
        Assert.StartsWith("removal failed", outcome.Why);
        Assert.Contains(_log, l => l.Contains("00000000000000c1 not removed"));
        Assert.Single(Directory.EnumerateFiles(actions));
        // the live launch's drain reads its own folder only: the ended launch's request is never run
        Assert.True(SfiBridgeFiles.EnqueueAction(Game, Launch, "world-editor", 3, "paste", null).Sent);
        var drain = new DrainModel(Actions, L, P) { Owners = { ["world-editor"] = 3 }, Registered = { "world-editor copy", "world-editor paste" } };
        drain.Turn();
        Assert.Equal(new[] { ("world-editor", "paste", (string?)null) }, drain.Calls);
    }

    [Fact]
    public void A_removal_that_fails_part_way_keeps_the_receipt_so_a_later_cleanup_finishes_it()
    {
        if (OperatingSystem.IsWindows())
        {
            Assert.Skip("skipped: a read-only folder does not stop deletion on Windows (covered by the Unix runs)");
            return;
        }
        SfiBridgeFiles.NowMs = () => new DateTimeOffset(Now).ToUnixTimeMilliseconds();
        SfiBridgeFiles.Liveness = (_, _) => SfiLiveness.Ended;
        var old = MakeLaunch("00000000000000c2", 100, 1000, 30);
        var staging = Path.Combine(old, "staging");
        File.Delete(Path.Combine(staging, "left.tmp"));
        Directory.CreateDirectory(Path.Combine(staging, "s"));  // a shorter path than launch.json's: removed after it by depth order
        Age(old, 30);
        File.SetUnixFileMode(staging, UnixFileMode.UserRead | UnixFileMode.UserExecute);
        var first = Assert.Single(SfiBridgeFiles.Cleanup(Game, L));
        File.SetUnixFileMode(staging, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        Assert.False(first.Removed);
        Assert.StartsWith("removal failed", first.Why);
        // the receipt is the only proof the launch ended: without it the half-removed folder would be kept for ever
        Assert.True(File.Exists(Path.Combine(old, "launch.json")), "the receipt goes last");
        Age(old, 30);  // a later start, still idle
        Assert.Equal((true, "ended"), SfiBridgeFiles.Cleanup(Game, L).Select(o => (o.Removed, o.Why)).Single());
        Assert.False(Directory.Exists(old));
    }

    [Fact]
    public void Cleanup_never_opens_its_lock_through_a_link()
    {
        SfiBridgeFiles.NowMs = () => new DateTimeOffset(Now).ToUnixTimeMilliseconds();
        SfiBridgeFiles.Liveness = (_, _) => SfiLiveness.Ended;
        MakeLaunch("00000000000000d2", 100, 1000, 30);
        var target = Path.Combine(_root, "elsewhere", "lock");
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        try { File.CreateSymbolicLink(Path.Combine(SfiBridgeFiles.UiRoot(Game), ".cleanup.lock"), target); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Assert.Skip("skipped: this system does not let a test make a file link"); }
        Assert.Equal(new[] { new SfiCleanupOutcome("", false, "a link on the path") }, SfiBridgeFiles.Cleanup(Game, L));
        Assert.False(File.Exists(target));
        Assert.True(Directory.Exists(SfiBridgeFiles.LaunchDir(Game, "00000000000000d2")));
    }

    [Fact]
    public void A_second_cleanup_at_the_same_time_does_nothing()
    {
        SfiBridgeFiles.NowMs = () => new DateTimeOffset(Now).ToUnixTimeMilliseconds();
        SfiBridgeFiles.Liveness = (_, _) => SfiLiveness.Ended;
        MakeLaunch("00000000000000d1", 100, 1000, 30);
        using (new FileStream(Path.Combine(SfiBridgeFiles.UiRoot(Game), ".cleanup.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
            Assert.Equal(new[] { new SfiCleanupOutcome("", false, "another cleanup runs") }, SfiBridgeFiles.Cleanup(Game, L));
        Assert.Equal((true, "ended"), SfiBridgeFiles.Cleanup(Game, L).Select(o => (o.Removed, o.Why)).Single());
    }

    [Fact]
    public void A_process_is_alive_only_as_the_start_its_receipt_names()
    {
        using var self = System.Diagnostics.Process.GetCurrentProcess();
        var start = new DateTimeOffset(self.StartTime.ToUniversalTime()).ToUnixTimeMilliseconds();
        Assert.Equal(SfiLiveness.Alive, SfiBridgeFiles.ProcessLiveness(Environment.ProcessId, start));
        Assert.Equal(SfiLiveness.Ended, SfiBridgeFiles.ProcessLiveness(Environment.ProcessId, start - 3_600_000));
        var gone = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(OperatingSystem.IsWindows() ? "cmd.exe" : "true") { Arguments = OperatingSystem.IsWindows() ? "/c exit 0" : "", UseShellExecute = false })!;
        gone.WaitForExit();
        var pid = gone.Id;
        gone.Dispose();
        Assert.Equal(SfiLiveness.Ended, SfiBridgeFiles.ProcessLiveness(pid, start));
    }
}

/// <summary>The runtime's reading of actions/ as drain/cases.json "rules" says it, on real files.</summary>
internal sealed class DrainModel(string dir, string launchId, string planHash)
{
    public Dictionary<string, long> Owners { get; } = [];
    public HashSet<string> Registered { get; } = [];
    public HashSet<string> Failing { get; } = [];
    public HashSet<string> RemoveFails { get; } = [];
    public List<(string Mod, string Action, string? Value)> Calls { get; } = [];
    public int Malformed { get; private set; }
    /// <summary>Entries removed without a call because they could not be read, and why.</summary>
    public List<string> Unread { get; } = [];
    /// <summary>Entries left for a later turn because another program held them without sharing.</summary>
    public List<string> Locked { get; } = [];
    /// <summary>Entries whose callback ran (a removal still pending on Windows can list one again, unreadable).</summary>
    public HashSet<string> CalledEntries { get; } = [];

    /// <summary>One turn; the number of entries examined.</summary>
    public int Turn()
    {
        if (!Directory.Exists(dir)) return 0;
        var entries = Directory.EnumerateFileSystemEntries(dir).Take(SfiBridgeFiles.EntriesPerTurn).ToList();
        foreach (var path in entries) Examine(path);
        return entries.Count;
    }

    private void Remove(string path)
    {
        if (RemoveFails.Contains(Path.GetFileName(path))) return;
        try { File.Delete(path); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    // Windows: ERROR_SHARING_VIOLATION or ERROR_LOCK_VIOLATION, what the runtime checks (raw_os_error 32 or 33). Elsewhere
    // .NET's advisory lock stands in for one: EWOULDBLOCK (Linux 11, macOS 35) is its sharing-violation error.
    private static bool SharingViolation(IOException e) => OperatingSystem.IsWindows()
        ? (e.HResult & 0xFFFF) is 32 or 33
        : e.HResult == (OperatingSystem.IsMacOS() ? 35 : 11);

    private void Examine(string path)
    {
        byte[] bytes;
        try
        {
            if (Directory.Exists(path)) throw new IOException("a folder");
            if (new FileInfo(path).Length > SfiBridgeFiles.RecordBytes) { Remove(path); return; }
            // as Rust's std opens a file on Windows: sharing read, write and delete, so the writer's rename still in
            // flight (it holds the file for delete) or its count of pending requests never makes an entry unreadable
            using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var copy = new MemoryStream();
            f.CopyTo(copy);
            bytes = copy.ToArray();
        }
        catch (IOException e) when (SharingViolation(e))
        {
            Locked.Add(Path.GetFileName(path));  // stays, unread, for a later turn
            return;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Unread.Add($"{Path.GetFileName(path)}: {e.GetType().Name}: {e.Message}");
            Remove(path);
            return;
        }
        JsonDocument doc;
        try { doc = JsonDocument.Parse(bytes); }
        catch (JsonException) { Malformed++; Remove(path); return; }
        using (doc)
        {
            var r = doc.RootElement;
            static bool Is(JsonElement r, string name, JsonValueKind kind, out JsonElement v) => r.TryGetProperty(name, out v) && v.ValueKind == kind;
            if (r.ValueKind != JsonValueKind.Object || !Is(r, "format", JsonValueKind.Number, out var f) || f.GetRawText() != "1" ||
                !Is(r, "kind", JsonValueKind.String, out var k) || k.GetString() != "action" ||
                !Is(r, "launchId", JsonValueKind.String, out var l) || l.GetString() != launchId ||
                !Is(r, "planHash", JsonValueKind.String, out var p) || p.GetString() != planHash ||
                !Is(r, "ownerEpoch", JsonValueKind.Number, out var ep) || !ep.TryGetInt64(out var epoch) ||
                !Is(r, "modId", JsonValueKind.String, out var m) || !Is(r, "action", JsonValueKind.String, out var a))
            {
                if (r.ValueKind != JsonValueKind.Object) Malformed++;
                Remove(path);
                return;
            }
            var (modId, action) = (m.GetString()!, a.GetString()!);
            var value = Is(r, "value", JsonValueKind.String, out var v) ? v.GetString() : null;
            if (!Owners.TryGetValue(modId, out var current) || current != epoch || !SfiBridgeFiles.SfId(modId) || !SfiBridgeFiles.SfId(action))
            {
                Remove(path);
                return;
            }
            if (!Registered.Contains($"{modId} {action}")) return;  // waits for its callback
            Calls.Add((modId, action, value));  // a failing callback is logged by the runtime, then removed as well
            CalledEntries.Add(Path.GetFileName(path));
            Remove(path);
        }
    }
}
