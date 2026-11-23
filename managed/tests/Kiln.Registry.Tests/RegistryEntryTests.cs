// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Kiln;
using Kiln.Registry;
using DataRegistry = Kiln.Registry.Registry;
using Xunit;
namespace Kiln.RegistryTests;
public sealed class RegistryEntryTests
{
    private static RegistryContext Frame(RegistryPhase phase, RegistryEntryPoint point = RegistryEntryPoint.GameStart, bool readOnly = false)
        => new(HostAccess.CreateContext(new ModInfo("test", "Test", "1", "."), 77, "."), point, phase, null, "entries",
            (_, _, field) => field is "inputs" or "outputs" ? "[{\"item\":\"game:wood\",\"count\":1}]" : field == "perks" ? "[]" : "10", (_, _) => ["game:wood"]) { ReadOnly = readOnly };
    [Fact]
    public void AddLikeBuffersOneStableDefinitionAndProjectsItsOwnTypedFields()
    {
        var context = Frame(RegistryPhase.Add); using var scope = context.Enter();
        var view = DataRegistry.For(context); ItemEntry? retained = null;
        view.Add(stage => stage.Items.AddLike(new("test:logs"), new("game:wood"), item => { retained = item; Assert.Equal(10, item.MaxStack); item.MaxStack = 500; }));
        var ir = JsonNode.Parse(context.CaptureIr()!)!;
        Assert.Equal("add", ir["stage"]!.GetValue<string>()); Assert.Single(ir["edits"]!.AsArray());
        Assert.Equal("test:logs", ir["edits"]![0]!["id"]!.GetValue<string>());
        Assert.Equal("game:wood", ir["edits"]![0]!["like"]!.GetValue<string>());
        Assert.Equal(500, ir["edits"]![0]!["fields"]!["maxStack"]!.GetValue<int>());
        context.Close(); Assert.Equal(RegistryContextError.Closed, Assert.Throws<RegistryContextException>(() => retained!.MaxStack).Code);
    }
    [Fact]
    public void AddConfigureListsAreFoldedIntoTheDefinitionAndFaultsPublishNothing()
    {
        var context = Frame(RegistryPhase.Add); using var scope = context.Enter(); var view = DataRegistry.For(context);
        Assert.Throws<InvalidOperationException>(() => view.Add(stage => stage.Items.AddLike(new("test:fault"), new("game:wood"), _ => throw new InvalidOperationException("fault"))));
        Assert.Null(context.CaptureIr());
        view.Add(stage => stage.Recipes.AddLike(new("test:recipe"), new("game:recipe"), recipe => {
            recipe.Inputs.Clear(); var input = recipe.Inputs.AppendNew(); input.Item = new RegistryId("game:wood"); input.Count = 2;
        }));
        var ir = JsonNode.Parse(context.CaptureIr()!)!; Assert.Single(ir["edits"]!.AsArray());
        Assert.Equal(2, ir["edits"]![0]!["fields"]!["inputs"]![0]!["count"]!.GetValue<int>()); context.Close();
    }
    [Fact]
    public void AddNamespacesAndReadOnlyContextsFailBeforeRecording()
    {
        var context = Frame(RegistryPhase.Add); using var scope = context.Enter(); var view = DataRegistry.For(context);
        Assert.Throws<ArgumentException>(() => view.Add(stage => stage.Items.AddLike(new("other:logs"), new("game:wood"), _ => { })));
        Assert.Null(context.CaptureIr()); context.Close();
        var readOnly = Frame(RegistryPhase.Add, readOnly: true); using var readScope = readOnly.Enter();
        var resolved = DataRegistry.For(readOnly);
        Assert.Equal(RegistryContextError.ReadOnly, Assert.Throws<RegistryContextException>(() => resolved.Add(stage => stage.Items.AddLike(new("test:logs"), new("game:wood"), _ => { }))).Code);
        Assert.Null(readOnly.CaptureIr()); readOnly.Close();
    }
    [Fact]
    public void LocalCloneCyclesFailBeforeConfigureCanRecursivelyReadThem()
    {
        var context = Frame(RegistryPhase.Add); using var scope = context.Enter(); var view = DataRegistry.For(context);
        view.Add(stage => stage.Items.AddLike(new("test:a"), new("test:b"), _ => { }));
        var configured = false;
        Assert.Throws<ArgumentException>(() => view.Add(stage => stage.Items.AddLike(new("test:b"), new("test:a"), _ => configured = true)));
        Assert.False(configured); Assert.Single(JsonNode.Parse(context.CaptureIr()!)!["edits"]!.AsArray()); context.Close();
    }
    [Fact]
    public void RemoveUsesHiddenEntryIrAndFinalKeepsItsPhase()
    {
        var context = Frame(RegistryPhase.Final); using var scope = context.Enter(); var view = DataRegistry.For(context);
        view.Final(stage => { stage.Items.Remove("game:wood"); stage.Recipes.Remove("game:recipe"); stage.Loot.Remove("game:wood"); });
        var ir = JsonNode.Parse(context.CaptureIr()!)!; Assert.Equal("final", ir["stage"]!.GetValue<string>());
        Assert.Equal(3, ir["edits"]!.AsArray().Count);
        Assert.All(ir["edits"]!.AsArray(), edit => Assert.Equal("remove", edit!["entry"]!.GetValue<string>())); context.Close();
    }
    [Fact]
    public void LaterPointsRecordEntryIntentSoNativeCanReturnRestartRequired()
    {
        var context = Frame(RegistryPhase.Alter, RegistryEntryPoint.Manual); using var scope = context.Enter(); var view = DataRegistry.For(context);
        view.Add(stage => stage.Items.AddLike(new("test:logs"), new("game:wood"), item => item.MaxStack = 50));
        view.Items.Remove("game:wood");
        Assert.Equal(2, JsonNode.Parse(context.CaptureIr()!)!["edits"]!.AsArray().Count); context.Close();
    }
}
