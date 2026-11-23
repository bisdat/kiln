// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

/// <summary>kiln/staged/probation (docs/kiln/staged-updates.md): Kiln.Host's verdict on a mod folder kiln-core updated at
/// this start ("loading" while its code runs, gone when it loaded, "failed" with the reason when it did not, so the next
/// start rolls it back once), and the confirmation of a Kiln update once the mods loaded. The records' format is the one
/// native/core/src/staged.cpp writes and reads (its host test drives the rollback that follows).</summary>
public sealed class StagedProbationTests : ModsFixture
{
    private string Staged => Path.Combine(Root, "staged");
    private string ProbationFile(string folder) => Path.Combine(Staged, "probation", $"mod.{folder}.json");

    private void Watch(string folder, string id, string state = "pending")
    {
        Directory.CreateDirectory(Path.Combine(Staged, "probation"));
        File.WriteAllText(ProbationFile(folder), new JsonObject
        {
            ["format"] = 1, ["unit"] = "mod." + folder, ["id"] = id, ["version"] = "2.0.0", ["from"] = "1.0.0", ["state"] = state,
            ["applied"] = "2026-11-23T10:00:00.000Z",
        }.ToJsonString());
    }

    private JsonObject? Record(string folder) =>
        File.Exists(ProbationFile(folder)) ? JsonNode.Parse(File.ReadAllText(ProbationFile(folder))) as JsonObject : null;

    private (ModManager Mods, FakeHost Host, StagedProbation Probation) Start()
    {
        var host = new FakeHost();
        var probation = new StagedProbation(Staged, ModsDir, host.Log);
        var mods = NewManager(host, new ModManagerOptions
        {
            KilnVersion = host.Version, HasFeature = host.HasFeature, StateFile = Path.Combine(LogsDir, "mods.txt"),
            TestResultsDirectory = Path.Combine(LogsDir, "tests"), Probation = probation,
        });
        return (mods, host, probation);
    }

    [Fact]
    public void A_mod_that_loads_after_its_update_is_kept()
    {
        InstallHello();
        Watch("hello", "hello");
        string? during = null;
        var (mods, host, probation) = Start();
        host.OnLine = line => { if (line.Contains("hello from C#", StringComparison.Ordinal)) during = Record("hello")?["state"]?.GetValue<string>(); };
        Assert.Equal(new[] { "hello" }, probation.Watched);
        mods.LoadAll();
        Assert.Equal("loading", during);  // a start that ended inside OnLoad would leave this for the next start
        Assert.Null(Record("hello"));
        Assert.True(host.Has("staged: mod hello 2.0.0 loaded after its update; it stays"), host.Dump());
        Assert.Empty(probation.Watched);
        mods.UnloadAll("test");
    }

    [Fact]
    public void A_mod_whose_OnLoad_throws_right_after_its_update_is_marked_failed()
    {
        Install("SampleFaulty", "sample-faulty");
        Directory.CreateDirectory(Path.Combine(DataDir, "sample-faulty"));
        File.WriteAllText(Path.Combine(DataDir, "sample-faulty", "missing-member"), "");
        Watch("sample-faulty", "sample-faulty");
        var (mods, host, _) = Start();
        mods.LoadAll();
        var r = Record("sample-faulty");
        Assert.NotNull(r);
        Assert.Equal("failed", r!["state"]!.GetValue<string>());
        Assert.Contains("Kiln.StatusLook", r["error"]!.GetValue<string>(), StringComparison.Ordinal);
        Assert.Equal("2.0.0", r["version"]!.GetValue<string>());  // the rest of kiln-core's record is kept
        Assert.Equal("mod.sample-faulty", r["unit"]!.GetValue<string>());
        Assert.True(host.Has("staged: mod sample-faulty failed to load right after its update to 2.0.0:"), host.Dump());
        Assert.True(host.Has("Kiln rolls it back to the version it replaced at the next start (once)"), host.Dump());
        mods.UnloadAll("test");
    }

    [Fact]
    public void A_mod_the_resolver_refuses_is_marked_failed_but_its_dependents_are_not()
    {
        WriteManifest("broken", """{"id": "broken", "version": "2.0.0", "apiVersion": "9.0"}""");
        WriteManifest("child", """{"id": "child", "version": "1.0.0", "apiVersion": "0.1", "dependencies": {"broken": "*"}}""");
        Watch("broken", "broken");
        Watch("child", "child");
        var (mods, _, probation) = Start();
        mods.LoadAll();
        Assert.Equal("failed", Record("broken")!["state"]!.GetValue<string>());
        Assert.Contains("needs Kiln API 9.0", Record("broken")!["error"]!.GetValue<string>(), StringComparison.Ordinal);
        // "dependency broken ... did not load": the dependency answers for it, so the child is not rolled back
        Assert.Equal("pending", Record("child")!["state"]!.GetValue<string>());
        Assert.Contains("child", probation.Watched);
    }

    [Fact]
    public void A_switched_off_mod_waits_for_its_first_load()
    {
        InstallHello();
        File.WriteAllText(Path.Combine(ModsDir, "hello", ".disabled"), "");
        Watch("hello", "hello");
        var (mods, _, _) = Start();
        mods.LoadAll();
        Assert.Equal("pending", Record("hello")!["state"]!.GetValue<string>());
    }

    [Fact]
    public void Only_pending_records_of_kiln_mods_folders_are_watched()
    {
        Watch("a", "a", "failed");     // the core rolls these back before Kiln.Host starts
        Watch("b", "b", "loading");
        Watch("c", "c");
        File.WriteAllText(ProbationFile("d"), """{"format":1,"unit":"mod.other","state":"pending"}""");  // not its own name
        File.WriteAllText(ProbationFile("e"), "{not json");
        var host = new FakeHost();
        var p = new StagedProbation(Staged, ModsDir, host.Log);
        Assert.Equal(new[] { "c" }, p.Watched);
        // a folder of the same name elsewhere is not the watched one
        p.Loaded(Path.Combine(Root, "elsewhere", "c"));
        Assert.NotNull(Record("c"));
        p.Loaded(Path.Combine(ModsDir, "c") + Path.DirectorySeparatorChar);
        Assert.Null(Record("c"));
        // no probation folder at all: nothing to watch, nothing written
        Directory.Delete(Path.Combine(Staged, "probation"), true);
        var none = new StagedProbation(Staged, ModsDir, host.Log);
        none.Failed(Path.Combine(ModsDir, "c"), "boom");
        none.ConfirmKiln();
        Assert.False(Directory.Exists(Path.Combine(Staged, "probation")));
    }

    [Theory]
    [InlineData("OnLoad threw InvalidOperationException: x", true)]
    [InlineData("needs Kiln API 9.0; this Kiln provides API 0.2", true)]
    [InlineData("missing dependency foo *", true)]  // the update added a dependency this server does not have
    [InlineData("dependency foo failed to load", false)]
    [InlineData("dependency foo * did not load (failed: needs Kiln API 9.0)", false)]
    public void Only_a_mods_own_failure_counts(string reason, bool blamed) => Assert.Equal(blamed, StagedProbation.Blames(reason));

    [Fact]
    public void A_Kiln_update_is_confirmed_once_the_mods_loaded()
    {
        Directory.CreateDirectory(Path.Combine(Staged, "probation"));
        var file = Path.Combine(Staged, "probation", "kiln.json");
        File.WriteAllText(file, """{"format":1,"unit":"kiln","version":"0.2.0-dev.30.10","from":"0.2.0-dev.30.9","state":"booting"}""");
        var host = new FakeHost();
        new StagedProbation(Staged, ModsDir, host.Log).ConfirmKiln();
        Assert.False(File.Exists(file));
        Assert.True(host.Has("staged: the Kiln update to 0.2.0-dev.30.10 is confirmed (the mods loaded); it stays"), host.Dump());
    }
}
