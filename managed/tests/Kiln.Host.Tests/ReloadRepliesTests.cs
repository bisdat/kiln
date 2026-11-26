// SPDX-License-Identifier: MIT
// The kiln/control `reload <id>` reply says what the reload left waiting for a restart in the registry (0.2.0-dev.31.14,
// ReloadReplies): before, it was "reloaded <id> <version>" and only registry.json and kiln.log had the RestartRequired rows.
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
    }
    private static bool _queued;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ContentStatus(byte* status)
    {
        if (*(uint*)status != 208) return KilnAbi.ErrInvalidArgument;  // KilnContentStatus is 208 bytes
        status[7] = _queued ? (byte)1 : (byte)0;                         // reload_pending at offset 7
        return 0;
    }
}
