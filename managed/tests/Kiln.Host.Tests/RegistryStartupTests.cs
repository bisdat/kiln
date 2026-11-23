// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using System.Text.Json.Nodes;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

// Loaded from this assembly's bytes into a real collectible author context, as BootConfigTests does.
public sealed class RegistryStartupAuthor : Mod
{
    public override void OnRegistry(RegistryContext context)
    {
        Log.Info("registry startup fixture: " + context.Phase);
        if (context.Phase == RegistryPhase.Alter)
            context.Capture = () => new JsonObject { ["ir"] = 1, ["mod"] = context.ModId, ["stage"] = "alter",
                ["edits"] = new JsonArray(new JsonObject { ["registry"] = "items", ["select"] = "game:wood",
                    ["field"] = "maxStack", ["op"] = 20 }) }.ToJsonString();
    }
}
public sealed class RegistryFaultingAuthor : Mod
{
    public override void OnRegistry(RegistryContext context) => throw new InvalidOperationException("registry startup fixture fault");
}

public sealed class RegistryExportAuthor : Mod
{
    public RegistryExportAuthor() => _ = Type.GetType("Fixture.Export, RuntimeOnlyExport", throwOnError: true);
    public override void OnRegistry(RegistryContext context) { }
}

public sealed class RegistryStartupTests : ModsFixture
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void FailingRegistryStartupDoesNotPreventOrdinaryManagedModsFromLoading(bool status)
    {
        InstallHello();
        using var native = new FakeRegistry { Phase = 0 };
        if (status) native.StageError = KilnAbi.ErrUnavailable;
        else native.ReadyError = KilnAbi.ErrUnavailable;
        using var manager = NewManager(native.Host);
        Assert.Null(Record.Exception(() => manager.PrepareRegistry(100)));
        native.Phase = 4;
        Assert.Null(Record.Exception(() => { manager.LoadAll(); }));
        Assert.NotNull(manager.Find("hello"));
        manager.Pre(Tick(1), []);
        Assert.True(native.Host.Has("registry:"), native.Host.Dump());
    }
    private string InstallAuthor(string id, Type type)
    {
        var manifest = new JsonObject { ["id"] = id, ["name"] = id, ["version"] = "1.0.0", ["apiVersion"] = "0.2",
            ["side"] = "server", ["entryAssembly"] = "Kiln.Host.Tests.dll", ["entryType"] = type.FullName };
        var directory = WriteManifest(id, manifest.ToJsonString());
        File.Copy(typeof(RegistryStartupTests).Assembly.Location, Path.Combine(directory, "Kiln.Host.Tests.dll"));
        return directory;
    }
    [Fact]
    public void ARealCollectibleExportRefusalKeepsItsActionableHintThroughLoaderWrappers()
    {
        var dependency = InstallAuthor("dependency", typeof(RegistryStartupAuthor));
        var dependencyFile = Path.Combine(dependency, Kiln.Host.Manifest.FileName);
        var dependencyManifest = JsonNode.Parse(File.ReadAllText(dependencyFile))!;
        dependencyManifest["exports"] = new JsonArray("RuntimeOnlyExport.dll");
        File.WriteAllText(dependencyFile, dependencyManifest.ToJsonString());
        File.Copy(typeof(RegistryStartupTests).Assembly.Location, Path.Combine(dependency, "RuntimeOnlyExport.dll"));
        var author = InstallAuthor("export-author", typeof(RegistryExportAuthor));
        var authorFile = Path.Combine(author, Kiln.Host.Manifest.FileName);
        var authorManifest = JsonNode.Parse(File.ReadAllText(authorFile))!;
        authorManifest["dependencies"] = new JsonObject { ["dependency"] = "*" };
        File.WriteAllText(authorFile, authorManifest.ToJsonString());
        File.Copy(typeof(RegistryStartupTests).Assembly.Location, Path.Combine(author, "RuntimeOnlyExport.dll")); // private copy cannot bypass the export guard
        using var native = new FakeRegistry { Phase = 0 }; using var manager = NewManager(native.Host);
        manager.PrepareRegistry(100);
        Assert.True(native.Host.Has("export-author game-start author"), native.Host.Dump());
        Assert.True(native.Host.Has("ModLoad or later"), native.Host.Dump());
        Assert.Equal(2, native.Subscriptions); // failed author retired; dependency and ready remain
    }
    [Fact]
    public void MalformedCandidatesAndAStageFaultLeaveOtherCollectibleAuthorsRunning()
    {
        InstallAuthor("good", typeof(RegistryStartupAuthor));
        InstallAuthor("faulted", typeof(RegistryFaultingAuthor));
        var garbage = InstallAuthor("garbage", typeof(RegistryStartupAuthor));
        File.WriteAllBytes(Path.Combine(garbage, "Kiln.Host.Tests.dll"), [1, 2, 3, 4]);
        var truncated = InstallAuthor("truncated", typeof(RegistryStartupAuthor));
        File.WriteAllBytes(Path.Combine(truncated, "Kiln.Host.Tests.dll"), File.ReadAllBytes(typeof(RegistryStartupTests).Assembly.Location)[..128]);
        using var native = new FakeRegistry { Phase = 0 };
        using var manager = NewManager(native.Host);
        manager.PrepareRegistry(100);
        Assert.Equal(3, native.Subscriptions); // ready, good author, faulting author; malformed PE files never load
        native.FireStage(RegistryPhase.Add);
        Assert.Equal(2, native.Subscriptions);
        native.FireStage(RegistryPhase.Alter);
        native.FireStage(RegistryPhase.Final);
        Assert.Equal(1, native.Submits);
        Assert.True(native.Host.Has("registry startup fixture: Final"), native.Host.Dump());
        Assert.True(native.Host.Has("registry startup fixture fault"), native.Host.Dump());
        native.Phase = 4; native.FireReady();
        manager.Pre(Tick(1), []);
        Assert.Equal(1, native.Subscriptions);
        Assert.Equal(2, native.Host.Revoked.Count); // both collectible author owners retired, not the ready owner
    }
}
