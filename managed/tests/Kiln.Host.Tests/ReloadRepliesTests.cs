// SPDX-License-Identifier: MIT
// The kiln/control `reload <id>` reply says what the reload left waiting for a restart in the registry (ReloadReplies): before, it was "reloaded <id> <version>" and only registry.json and kiln.log had the RestartRequired rows.
// The reply waits for the reload's registry work (its runtime pass, a queued data-pack application), at most 5 s, then names
// the count and each mod and field of registry.json's rows with the stable token, for the reloaded mods only; data.registry
// carries them for tools (the Web Admin mod reads it).
#pragma warning disable KILNX031
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed unsafe class ReloadRepliesTests : ModsFixture
{
    private const string Report = """
        {"format": 1, "refused": [
          {"mod": "bigpack", "source": "content/a.json", "stage": "alter", "n": 1, "registry": "balance", "id": "*", "field": "levelCap",
           "reason": "RestartRequired: balance[*].levelCap is read at game start; the reloaded value applies at the next start"},
          {"mod": "bigpack", "source": "content/a.json", "stage": "alter", "n": 2, "registry": "items", "id": "game:wood", "field": "levels.min",
           "reason": "RestartRequired: items.levels.min: the base was read at start; this edit was not applied"},
          {"mod": "bigpack", "source": "content/a.json", "stage": "alter", "n": 3, "registry": "items", "id": "game:wood", "field": "maxStack",
           "reason": "unknown field"},
          {"mod": "other", "source": "content/o.json", "stage": "alter", "n": 0, "registry": "items", "id": "game:stone", "field": "maxStack",
           "reason": "RestartRequired: a registry section added after the start needs a restart; this edit was not applied"},
          {"mod": "lib", "source": "live:4:2:0:x", "stage": "alter", "n": 0, "registry": "items", "id": "game:wood", "field": "x", "raw": true,
           "reason": "RestartRequired: the base did not read items.x at start"}
        ]}
        """;

    [Fact]
    public void OnlyTheReloadedModsRowsWithTheTokenAreNamedWithTheirPlaces()
    {
        var o = ReloadReplies.Of(Report, ["bigpack", "lib"]);
        Assert.True(o.Known);
        Assert.Equal(3, o.Total);  // bigpack's two restart rows and lib's; not the plain refusal, not the other mod's
        Assert.Equal(new[] { "balance.levelCap", "items[game:wood].levels.min", "items[game:wood].(raw path)" }, o.Edits.Select(e => e.Place));
        var suffix = ReloadReplies.Suffix(o);
        Assert.Contains("; registry: 3 edits need a restart: bigpack balance.levelCap, bigpack items[game:wood].levels.min, lib items[game:wood].(raw path)", suffix);
        Assert.Contains("kiln/state/registry.json", suffix);
        Assert.Equal("", ReloadReplies.Suffix(ReloadReplies.Of(Report, ["nothing-here"])));
        Assert.Equal("", ReloadReplies.Suffix(ReloadReplies.Of(null, ["bigpack"])));   // no registry: the reply as before
        Assert.Equal("", ReloadReplies.Suffix(ReloadReplies.Of("{not json", ["bigpack"])));
        var one = ReloadReplies.Suffix(ReloadReplies.Of(Report, ["other"]));
        Assert.Contains("1 edit needs a restart: other items[game:stone].maxStack", one);
    }

    [Fact]
    public void ManyEditsAreCountedAndTheFirstEightNamed()
    {
        var rows = new JsonArray();
        for (var i = 0; i < 12; ++i)
            rows.Add(new JsonObject { ["mod"] = "m", ["source"] = "content/a.json", ["stage"] = "alter", ["n"] = i, ["registry"] = "balance",
                ["id"] = "*", ["field"] = $"f{i}", ["reason"] = "RestartRequired: x" });
        var o = ReloadReplies.Of(new JsonObject { ["refused"] = rows }.ToJsonString(), ["m"]);
        Assert.Equal(12, o.Total);
        var suffix = ReloadReplies.Suffix(o);
        Assert.Contains("12 edits need a restart: m balance.f0", suffix);
        Assert.Contains("m balance.f7 and 4 more", suffix);
        Assert.DoesNotContain("f8", suffix);
    }

    [Fact]
    public void TheReplyWaitsForTheRegistryWorkThenNamesWhatWaitsForARestart()
    {
        var replies = new ReloadReplies();
        var written = new List<(string File, bool Ok, string Message, JsonNode Data)>();
        void Write(string file, string command, bool ok, string message, object data) =>
            written.Add((file, ok, message, JsonNode.Parse(System.Text.Json.JsonSerializer.Serialize(data))!));
        var pending = true;
        var reads = 0;
        replies.Add("r.result.json", "reload bigpack", new ModOperationResult(true, "reloaded bigpack 1.0.1", ["bigpack"]), ["bigpack"], now: 0);
        for (var step = 0; step < 10; ++step) replies.Step(step * 16, _ => pending, () => { ++reads; return Report; }, Write);
        Assert.Empty(written);  // its runtime pass or data packs still wait
        Assert.Equal(0, reads);
        pending = false;
        replies.Step(200, _ => pending, () => { ++reads; return Report; }, Write);
        var (file, ok, message, data) = Assert.Single(written);
        Assert.Equal("r.result.json", file);
        Assert.True(ok);
        Assert.StartsWith("reloaded bigpack 1.0.1; registry: 2 edits need a restart: bigpack balance.levelCap", message);
        Assert.Equal(1, reads);
        Assert.Equal(2, data["registry"]!["restartRequired"]!.GetValue<int>());
        Assert.True(data["registry"]!["settled"]!.GetValue<bool>());
        Assert.Equal("levelCap", data["registry"]!["edits"]![0]!["field"]!.GetValue<string>());
        Assert.Equal("bigpack", data["affected"]![0]!.GetValue<string>());
        Assert.Equal(0, replies.Count);
    }

    [Fact]
    public void AReplyNeverWaitsLessThanItsStepsNorLongerThanItsTimeout()
    {
        var replies = new ReloadReplies();
        var written = new List<string>();
        replies.Add("a", "reload a", new ModOperationResult(true, "reloaded a 1", ["a"]), ["a"], now: 0);
        for (var step = 1; step < ReloadReplies.MinSteps; ++step) replies.Step(step, _ => false, () => Report, (_, _, _, m, _) => written.Add(m));
        Assert.Empty(written);  // the reload's event and kiln.content's Pre have not come yet
        replies.Step(ReloadReplies.MinSteps, _ => false, () => Report, (_, _, _, m, _) => written.Add(m));
        Assert.Equal("reloaded a 1", Assert.Single(written));  // nothing of a's waits for a restart: the reply as before

        written.Clear();
        replies.Add("b", "reload bigpack", new ModOperationResult(true, "reloaded bigpack 2", ["bigpack"]), ["bigpack"], now: 0);
        for (var t = 0L; t < (long)ReloadReplies.Timeout.TotalMilliseconds; t += 1000) replies.Step(t, _ => true, () => Report, (_, _, _, m, _) => written.Add(m));
        Assert.Empty(written);
        replies.Step((long)ReloadReplies.Timeout.TotalMilliseconds, _ => true, () => Report, (_, _, _, m, _) => written.Add(m));
        var late = Assert.Single(written);
        Assert.Contains("; registry: its edits were not all applied yet", late);  // the world is not ready: said, with what the report has now
        Assert.Contains("2 edits need a restart", late);

        written.Clear();
        replies.Add("c", "reload bigpack", new ModOperationResult(true, "reloaded bigpack 3", ["bigpack"]), ["bigpack"], now: 0);
        replies.Step(long.MaxValue, _ => false, () => throw new KilnUnavailableException("kiln.registry"), (_, _, _, m, _) => written.Add(m));
        Assert.Equal("reloaded bigpack 3", Assert.Single(written));  // a stop: written at once; a registry that cannot answer leaves it as before
    }

    [Fact]
    public void TheHostReadsTheReportAndWaitsForAPendingRuntimePassAndAQueuedPackApplication()
    {
        using var native = new FakeRegistry();
        using var manager = NewManager(native.Host);
        manager.PrepareRegistry(100);
        var dispatch = Assert.IsType<RegistryDispatch>(RegistryRuntime.Host);
        Assert.True(manager.RegistryAvailable);
        native.ReportText = Report;
        Assert.Equal(Report, manager.RegistryReportJson());
        var owner = HostAccess.CreateContext(new ModInfo("bigpack", "bigpack", "1.0.0", Root), native.Host.OwnerCreate("bigpack"), DataDir);
        dispatch.Loaded(new Author(), owner, Root, true);
        Assert.True(manager.RegistryWorkPending(["bigpack"]));   // its HotReload point waits for the next tick
        Assert.False(manager.RegistryWorkPending(["other"]));
        dispatch.FlushLoads();
        Assert.False(manager.RegistryWorkPending(["bigpack"]));
        // kiln.content's status says a data-pack application is queued for its next Pre (a reload's packs apply there)
        var table = (ContentTable*)NativeMemory.AllocZeroed((nuint)sizeof(ContentTable));
        try
        {
            *table = new ContentTable { Size = 288, Version = 4, Status = &ContentStatus };
            native.Host.Services["kiln.content"] = (nint)table;
            _queued = true;
            Assert.True(manager.RegistryWorkPending(["bigpack"]));
            _queued = false;
            Assert.False(manager.RegistryWorkPending(["bigpack"]));
        }
        finally { native.Host.Services.Remove("kiln.content"); NativeMemory.Free(table); }
        owner.Retire();
    }

    // registry-fixes item 3 (review-registry-reload Q2): a data pack that threw while the reload's application applied it set its
    // mod apart (what it did came off; its packs from before apply again, or none), and only kiln.log and Content.PackReport()
    // said so: the reply said "reloaded <id>". kiln.content's pack report lists the mods set apart ("setApart"); the reply names
    // the reloaded mods' rows of the application after the reload, and data.packs carries them. Additive: message prefix and
    // data.registry as before.
    private const string Packs = """
        {"generation": 7, "packs": [{"file": "undo-lab/content/a.json", "applied": 2, "errors": [], "warnings": []}], "skipped": [], "conflicts": [],
         "setApart": [
          {"mod": "undo-lab", "file": "undo-lab/content/b.json", "reason": "[json.exception.type_error.302] type must be string, but is object", "previous": true},
          {"mod": "lib", "file": "lib/content/x.json", "reason": "boom", "previous": false},
          {"mod": "other", "file": "other/content/o.json", "reason": "x", "previous": true}]}
        """;

    [Fact]
    public void ADataPackThatThrewIsNamedForTheReloadedModsOfTheApplicationAfterTheReload()
    {
        var o = ReloadReplies.PacksOf(Packs, ["undo-lab", "lib"], after: 6);
        Assert.True(o.Known);
        Assert.Equal(new[] { "undo-lab", "lib" }, o.SetApart.Select(x => x.Mod));  // not the other mod's row
        var suffix = ReloadReplies.PacksSuffix(o);
        Assert.Equal("; data packs: undo-lab's undo-lab/content/b.json threw while it applied ([json.exception.type_error.302] type must be " +
                     "string, but is object) and was set apart: what it did came off and the packs it had before apply again; lib's " +
                     "lib/content/x.json threw while it applied (boom) and was set apart: what it did came off and none of its packs apply " +
                     "(kiln/logs/kiln.log has each line)", suffix);
        // an application from before the reload (its generation is not after the reload's): not this reload's
        Assert.Empty(ReloadReplies.PacksOf(Packs, ["undo-lab"], after: 7).SetApart);
        Assert.Equal("", ReloadReplies.PacksSuffix(ReloadReplies.PacksOf(Packs, ["undo-lab"], after: 7)));
        Assert.Equal("", ReloadReplies.PacksSuffix(ReloadReplies.PacksOf(Packs, ["nothing-here"], after: 0)));
        Assert.False(ReloadReplies.PacksOf(null, ["undo-lab"], after: 0).Known);         // no kiln.content: the reply as before
        Assert.False(ReloadReplies.PacksOf("{not json", ["undo-lab"], after: 0).Known);
        Assert.Empty(ReloadReplies.PacksOf("""{"generation": 9, "packs": []}""", ["undo-lab"], after: 0).SetApart);  // an older kiln.content
    }

    [Fact]
    public void TheReplyNamesASetApartPackAndItsDataCarriesIt()
    {
        var replies = new ReloadReplies();
        var written = new List<(bool Ok, string Message, JsonNode Data)>();
        void Write(string file, string command, bool ok, string message, object data) =>
            written.Add((ok, message, JsonNode.Parse(System.Text.Json.JsonSerializer.Serialize(data))!));
        var packReads = 0;
        replies.Add("r.result.json", "reload undo-lab", new ModOperationResult(true, "reloaded undo-lab 1.0.1", ["undo-lab"]), ["undo-lab"], now: 0,
            packsAfter: 6);
        replies.Add("s.result.json", "reload undo-lab", new ModOperationResult(true, "reloaded undo-lab 1.0.2", ["undo-lab"]), ["undo-lab"], now: 0);
        for (var step = 0; step <= ReloadReplies.MinSteps; ++step)
            replies.Step(step, _ => false, () => Report, _ => [], () => { ++packReads; return Packs; }, Write);
        Assert.Equal(2, written.Count);
        var (ok, message, data) = written[0];
        Assert.True(ok);  // the reload itself did what it says: the mod is loaded; its packs are what applies
        Assert.StartsWith("reloaded undo-lab 1.0.1; data packs: undo-lab's undo-lab/content/b.json threw while it applied", message);
        Assert.True(data["packs"]!["known"]!.GetValue<bool>());
        var row = data["packs"]!["setApart"]![0]!;
        Assert.Equal("undo-lab", row["mod"]!.GetValue<string>());
        Assert.Equal("undo-lab/content/b.json", row["file"]!.GetValue<string>());
        Assert.True(row["previous"]!.GetValue<bool>());
        Assert.NotNull(data["registry"]);   // the registry part as before
        Assert.Equal("undo-lab", data["affected"]![0]!.GetValue<string>());
        // a reply added without the content generation (an older kiln.content, or none) reads no pack report and says nothing of it
        var (_, plain, plainData) = written[1];
        Assert.Equal("reloaded undo-lab 1.0.2", plain);
        Assert.False(plainData["packs"]!["known"]!.GetValue<bool>());
        Assert.Equal(1, packReads);
    }

    // Kiln.Host reads kiln.content's generation (KilnContentStatus.generation, offset 40) and its pack report (report(), the
    // table's 18th function: offset 144) through the table, as it reads reload_pending
    [Fact]
    public void TheHostReadsTheContentGenerationAndThePackReport()
    {
        var host = new FakeHost();
        Assert.Null(ContentStatusProbe.Generation(host));
        Assert.Null(ContentStatusProbe.PackReport(host));
        var table = (ContentTable*)NativeMemory.AllocZeroed((nuint)sizeof(ContentTable));
        try
        {
            *table = new ContentTable { Size = 288, Version = 4, Status = &ContentStatus, Report = &ContentReport };
            host.Services["kiln.content"] = (nint)table;
            Assert.Equal(7UL, ContentStatusProbe.Generation(host));
            Assert.Equal(Packs, ContentStatusProbe.PackReport(host));
            table->Size = 144;  // a table without report(): no pack report, the generation still reads
            Assert.Null(ContentStatusProbe.PackReport(host));
            Assert.Equal(7UL, ContentStatusProbe.Generation(host));
        }
        finally { host.Services.Remove("kiln.content"); NativeMemory.Free(table); }
    }

    [RegistryEntryPoints(RegistryEntryPoint.ModLoad | RegistryEntryPoint.HotReload)]
    private sealed class Author : Mod
    {
        public override void OnRegistry(RegistryContext context) { }
    }
    // GameStart only (the default of a real override): a reload does not run its OnRegistry again
    private sealed class GameStartAuthor : Mod
    {
        public override void OnRegistry(RegistryContext context) { }
    }
    [RegistryEntryPoints(RegistryEntryPoint.GameStart | RegistryEntryPoint.HotReload)]
    private sealed class GameStartAndHotReload : Mod
    {
        public override void OnRegistry(RegistryContext context) { }
    }

    // Documented ("already committed GameStart layers stay until process restart, including through a runtime hot reload"),
    // but the reload said nothing: the new code ran with the start's registry edits. Now its reply says they change at the next start.
    [Fact]
    public void AReloadedGameStartOnlyAuthorSaysItsRegistryEditsWaitForTheNextStart()
    {
        using var native = new FakeRegistry();
        using var manager = NewManager(native.Host);
        manager.PrepareRegistry(100);
        var dispatch = Assert.IsType<RegistryDispatch>(RegistryRuntime.Host);
        ModContext Ctx(string id) => HostAccess.CreateContext(new ModInfo(id, id, "1.0.1", Root), native.Host.OwnerCreate(id), DataDir);
        var only = Ctx("startonly");
        dispatch.Loaded(new GameStartAuthor(), only, Root, reload: true);
        var (mod, reason) = Assert.Single(manager.RegistryRestartNotes(["startonly"]));
        Assert.Equal("startonly", mod);
        Assert.StartsWith("RestartRequired: startonly's OnRegistry runs at GameStart only", reason);
        Assert.True(native.Host.Has("RestartRequired: startonly's OnRegistry runs at GameStart only"), native.Host.Dump());
        var both = Ctx("both");
        dispatch.Loaded(new GameStartAndHotReload(), both, Root, reload: true);
        Assert.Empty(manager.RegistryRestartNotes(["both"]));  // its HotReload pass re-runs OnRegistry
        var loadedFirst = Ctx("fresh");
        dispatch.Loaded(new GameStartAuthor(), loadedFirst, Root, reload: false);
        Assert.Empty(manager.RegistryRestartNotes(["fresh"]));  // a first load: nothing it had at the start to replace
        var outcome = ReloadReplies.With(ReloadReplies.Of(Report, ["startonly"]), manager.RegistryRestartNotes(["startonly"]));
        Assert.Equal(1, outcome.Total);
        Assert.Contains("1 edit needs a restart: startonly OnRegistry (GameStart)", ReloadReplies.Suffix(outcome));
        only.Retire(); both.Retire(); loadedFirst.Retire();
    }


    [StructLayout(LayoutKind.Sequential)]
    private struct ContentTable
    {
        internal uint Size, Version;
        internal delegate* unmanaged[Cdecl]<byte*, int> Status;
        internal fixed long Before[16];  // count .. reload_packs
        internal delegate* unmanaged[Cdecl]<byte*, nuint, nuint*, int> Report;
    }
    private static bool _queued;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ContentStatus(byte* status)
    {
        if (*(uint*)status != 208) return KilnAbi.ErrInvalidArgument;  // KilnContentStatus is 208 bytes
        status[7] = _queued ? (byte)1 : (byte)0;                         // reload_pending at offset 7
        *(ulong*)(status + 40) = 7;                                      // generation at offset 40
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ContentReport(byte* buffer, nuint cap, nuint* len)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(Packs);
        *len = (nuint)bytes.Length;
        if (cap <= (nuint)bytes.Length) { if (cap > 0) { bytes.AsSpan(0, (int)cap - 1).CopyTo(new Span<byte>(buffer, (int)cap - 1)); buffer[cap - 1] = 0; } return 0; }  // cut at cap
        bytes.CopyTo(new Span<byte>(buffer, bytes.Length));
        buffer[bytes.Length] = 0;
        return 0;
    }
}
