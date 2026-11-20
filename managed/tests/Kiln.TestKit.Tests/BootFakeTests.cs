// SPDX-License-Identifier: MIT
// The kit's boot phase (Fakes/Boot/): a boot mod's OnBoot as a start runs it, then the mod loads with its boot content committed.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.TestKit.Tests;

[Collection("boot generation")]  // BootGeneration is process-wide
public sealed class BootFakeTests
{
    private static string Folder => TestMods.Folder("TkBoot");

    [Fact]
    public void OnBootStagesWithTheGamesDataAndTheModThenLoadsCommitted()
    {
        using var kiln = new KilnHarness();
        kiln.BootPhase.SetRecord("item:Material_T1_Wood_Logs", """{"maxStackSize": 120}""");
        var run = kiln.BootPhase.Run(Folder);
        Assert.True(run.Ok, run.Reason);
        Assert.Equal(BootModState.Committed, run.State);
        Assert.Equal(["statuses/tk-cold", "items/tk-token"], run.Staged.Select(d => $"{d.Kind}/{d.Key}"));
        Assert.Equal(["statuses/tk-cold", "items/tk-token"], run.Definitions);
        Assert.Contains("\"stack\":120", Assert.Single(run.Staged, d => d.Kind == "items").Json);
        var mod = kiln.LoadFolder(Folder);
        kiln.Boot();
        kiln.Step(3);
        Assert.True(mod.IsLoaded, mod.Error);
        Assert.Equal(BootModState.Committed, Mods.BootState("tk-boot").State);
        Assert.Contains(kiln.LogsOf("tk-boot"), l => l.Text == "tk-boot: committed statuses/tk-cold, items/tk-token in generation 1");
        Assert.Contains(Coverage.All, r => r.Capabilities.Contains("mods.boot") && r.Kind == CoverageKind.RecordingFake);
    }

    [Fact]
    public void AMissingRecordFailsTheRunAndTheModIsRefusedAsAtAStart()
    {
        using var kiln = new KilnHarness();
        var run = kiln.BootPhase.Run(Folder);  // nothing scripted: the read answers NotFound and OnBoot throws
        Assert.False(run.Ok);
        Assert.Equal(BootModState.ExcludedCallback, run.State);
        Assert.Contains("NotFound", run.Reason);
        Assert.Contains(run.Issues, i => i.Code == "CallbackFailed");
        Assert.Empty(run.Definitions);
        var mod = kiln.LoadFolder(Folder);
        var e = Assert.Throws<ModLoadException>(kiln.Boot);  // as at a start: its runtime instance is refused until a restart
        Assert.Contains("RestartRequiredBoot", e.Message);
        Assert.False(mod.IsLoaded);
    }

    [Theory]
    [InlineData("icons/missing.png", "\"icons/missing.png\" is not in the mod's folder ({0}). The build copies the mod's folders (KilnModFolders); the base icon stays")]
    [InlineData("../outside.png", "\"../outside.png\": a file in the mod's folder, with a relative path; the base icon stays")]
    public void ABootItemsUnusableIconIsOneErrorOnceTheModLoadsAsForARunTimeDefine(string icon, string why)
    {
        // what the SDK's bake reads (Kiln.Sdk.Bake: BootPhase.Run, LoadFolder, the steps, then the error lines): the boot lowering refuses the
        // icon against the mod's folder and keeps the item, silently; its runtime activation, a run-time define of the same block, says why once
        using var kiln = new KilnHarness();
        kiln.BootPhase.SetRecord("item:Material_T1_Wood_Logs", """{"maxStackSize": 120}""");
        Directory.CreateDirectory(kiln.ConfigDirectory);
        File.WriteAllText(Path.Combine(kiln.ConfigDirectory, "tk-boot.json"), $$"""{ "tokenIcon": "{{icon}}" }""");
        var run = kiln.BootPhase.Run(Folder);
        Assert.True(run.Ok, run.Reason);
        Assert.Equal(["statuses/tk-cold", "items/tk-token"], run.Definitions);
        Assert.Contains($"\"icon\":\"{icon}\"", Assert.Single(run.Staged, d => d.Kind == "items").Json);
        var mod = kiln.LoadFolder(Folder);
        kiln.Boot();
        kiln.Step(3);
        Assert.True(mod.IsLoaded, mod.Error);
        var line = Assert.Single(kiln.Logs, l => l.Text.Contains("visual.icon", StringComparison.Ordinal));
        Assert.Equal(LogLevel.Error, line.Level);
        var file = Path.GetFullPath(Path.Combine(mod.Folder, icon));
        Assert.Equal($"blocks: tk-boot: block item tk-token, visual.icon: {file}: {string.Format(why, file)}", line.Text);
    }

    [Fact]
    public void ASettingOnBootReadIsFrozenByItsValue()
    {
        using var kiln = new KilnHarness();
        kiln.BootPhase.SetRecord("item:Material_T1_Wood_Logs", """{"maxStackSize": 120}""");
        var file = Path.Combine(kiln.ConfigDirectory, "tk-boot.json");
        Directory.CreateDirectory(kiln.ConfigDirectory);
        File.WriteAllText(file, """{ "coldSeconds": 4 }""");  // not Kiln's canonical form: OnLoad writes that
        var run = kiln.BootPhase.Run(Folder);
        Assert.True(run.Ok, run.Reason);
        Assert.Contains("\"seconds\":4", Assert.Single(run.Staged, d => d.Kind == "statuses").Json);
        var mod = kiln.LoadFolder(Folder);
        kiln.Boot();
        kiln.Step(3);
        Assert.True(mod.IsLoaded, mod.Error);
        var canonical = File.ReadAllText(file);
        Assert.NotEqual("""{ "coldSeconds": 4 }""", canonical);  // the runtime registration rewrote it canonically

        // the canonical rewrite, and the same value written another way, load again (unload, then Mods.LoadAsync of its folder)
        ModOperationResult LoadAgain()
        {
            var task = Mods.LoadAsync("tk-boot");
            kiln.Step(2);
            return task.Result;
        }
        kiln.Unload(mod);
        var again = LoadAgain();
        Assert.True(again.Ok, again.Message);
        kiln.Unload(mod);
        File.WriteAllText(file, "// my settings\n{\n  \"coldSeconds\": 4,\n}\n");
        again = LoadAgain();
        Assert.True(again.Ok, again.Message);

        // another value: the boot content was made with 4
        kiln.Unload(mod);
        File.WriteAllText(file, """{ "coldSeconds": 5 }""");
        again = LoadAgain();
        Assert.False(again.Ok);
        Assert.StartsWith("RestartRequiredBoot:", again.Message);
        Assert.Contains("config tk-boot", again.Message);
        Assert.Contains("coldSeconds: 4 -> 5", again.Message);
        Assert.Contains("restart the game", again.Message);
    }

    [Fact]
    public void TheRowsSwitchedOffRefuseAsInAGame()
    {
        using var kiln = new KilnHarness();
        kiln.BootPhase.SetRecord("item:Material_T1_Wood_Logs", """{"maxStackSize": 120}""");
        kiln.Capabilities.Override("mods.boot.game_data", false);
        var run = kiln.BootPhase.Run(Folder, commit: false);
        Assert.False(run.Ok);
        Assert.Contains("mods.boot.game_data", run.Reason);
        kiln.Capabilities.Override("mods.boot.game_data", true);
        kiln.Capabilities.Override("mods.boot", false);
        run = kiln.BootPhase.Run(Folder, commit: false);
        Assert.Contains(run.Issues, i => i.Code == "BootCapOff");
        Assert.Empty(run.Staged);  // its OnBoot did not run
    }

    [Fact]
    public void AKilnSdkModOptsInWithKilnBootAlone()
    {
        // tk-boot-sdk has no kiln.mod.json of its own: <KilnBoot>true</KilnBoot> wrote "boot", and its build passed KILN034
        var folder = TestMods.Folder("TkBootSdk");
        var manifest = JsonNode.Parse(File.ReadAllText(Path.Combine(folder, "kiln.mod.json")))!.AsObject();
        Assert.Equal("tk-boot-sdk", (string?)manifest["id"]);
        Assert.True((bool?)manifest["boot"]);
        using var kiln = new KilnHarness();
        var run = kiln.BootPhase.Run(folder);
        Assert.True(run.Ok, run.Reason);
        Assert.Equal(BootModState.Committed, run.State);
        Assert.Equal(["statuses/tk-sdk-cold"], run.Definitions);
    }

    [Fact]
    public void NamesOfTemplatesWithAComponentCountTheTemplatesOwnComponentsOnly()
    {
        using var kiln = new KilnHarness();
        kiln.BootPhase.SetRecord("template:Zone", """{"components": [{"$type": "Health"}, {"$type": "PickupItemZone", "radius": 2}]}""");
        kiln.BootPhase.SetRecord("template:Nested", """{"components": [{"$type": "Spawner", "spawn": {"$type": "PickupItemZone"}}]}""");
        var data = new BootFake.KitGameData(BootFake.StateOf(kiln));
        string[] Names(string key)
        {
            var r = data.Read(key);
            Assert.True(r.Ok, r.Reason);
            return r.Data!.Value.GetProperty("names").EnumerateArray().Select(x => x.GetString()!).ToArray();
        }
        Assert.Equal(["Zone"], Names("names:templates/PickupItemZone"));
        Assert.Equal(["Nested"], Names("names:templates/Spawner"));
        Assert.Empty(Names("names:templates/PickupItem"));
        Assert.Equal(["Nested", "Zone"], Names("names:templates"));
    }

    [Fact]
    public void ResourceQueriesTakeAQualifiedTypeNameAndNamesResourcesListsTheScriptedResources()
    {
        using var kiln = new KilnHarness();
        var scene = Guid.Parse("509feadb-4c60-425f-9c7c-deeefd9b6920");
        var hash = BootFake.KitGameData.TypeHash("keen::SceneResource");
        Assert.Equal(0x2bc68ebeu, hash);  // the game's type hash of keen::SceneResource
        kiln.BootPhase.SetRecord($"resource:{scene}/{hash:x8}/0", """{"chunks": 3}""", scene, hash, 0);
        kiln.BootPhase.SetRecord("item:Zed", """{"debugName": "Zed"}""", Guid.Parse("00000000-0000-4000-8000-000000000002"), BootFake.KitGameData.TypeHash("keen::ItemInfo"), 0);
        var data = new BootFake.KitGameData(BootFake.StateOf(kiln));

        // resource: with the qualified name is the hash form
        var byName = data.Read($"resource:{scene}/keen::SceneResource/0");
        Assert.True(byName.Ok, byName.Reason);
        Assert.Equal(3, byName.Data!.Value.GetProperty("chunks").GetInt32());
        Assert.False(data.Read($"resource:{scene}/keen::SceneResource/1").Ok);
        Assert.Throws<ArgumentException>(() => data.Read($"resource:{scene}/SceneResource/0"));

        // names:resources/<type>: the scripted resources of that type, unnamed ones by their GUID
        var list = data.Read("names:resources/keen::SceneResource");
        Assert.True(list.Ok, list.Reason);
        Assert.Equal(hash, list.Data!.TypeHash);
        Assert.Equal([scene.ToString("D")], list.Data.Value.GetProperty("names").EnumerateArray().Select(x => x.GetString()!));
        var row = Assert.Single(list.Data.Value.GetProperty("resources").EnumerateArray());
        Assert.Equal((scene.ToString("D"), 0, ""), (row.GetProperty("resourceId").GetString(), row.GetProperty("part").GetInt32(), row.GetProperty("name").GetString()));
        var items = data.Read("names:resources/keen::ItemInfo");
        Assert.True(items.Ok, items.Reason);
        Assert.Equal(["Zed"], items.Data!.Value.GetProperty("names").EnumerateArray().Select(x => x.GetString()!));
        Assert.False(data.Read("names:resources/keen::BalancingTable").Ok);
        Assert.Throws<ArgumentException>(() => data.Read("names:resources/SceneResource"));
    }
}
