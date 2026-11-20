using System.Text.Json.Nodes;
using ClientPass;
using Xunit;
namespace Kiln.Contract.Tests;
public sealed class VfxRestartProofTests
{
    private static JsonObject Fixture(string process)
    {
        var effects = new JsonArray();
        foreach(var name in new[] { "budget_red", "budget_green", "budget_blue" }) effects.Add(new JsonObject {
            ["name"]=name,["registrationId"]=Guid.NewGuid().ToString(),["definition"]=VfxId.Named("vfx-test",name).Guid.ToString(),
            ["state"]="AcceptedNextStart",["ok"]=true,["slots"]=0 });
        return JsonNode.Parse(new JsonObject { ["fixture"]=VfxBudgetProof.Fixture,["process"]=process,["effects"]=effects,
            ["requireRegistered"]=new JsonObject { ["name"]="budget_never_stored",["ok"]=false,["slots"]=0,["code"]="RegistryClosed" }}.ToJsonString())!.AsObject();
    }
    [Fact] public void Before_requires_the_exact_three_names_and_refused_fourth()
    {
        var doc=Fixture("first"); Assert.True(VfxBudgetProof.Before(doc,"first",7,out _));
        doc["effects"]![0]!["name"]="unrelated"; Assert.False(VfxBudgetProof.Before(doc,"first",7,out _));
        doc=Fixture("first"); doc["requireRegistered"]!["ok"]=true; Assert.False(VfxBudgetProof.Before(doc,"first",7,out _));
        doc=Fixture("first"); doc["effects"]![0]!["state"]="Registered"; Assert.False(VfxBudgetProof.Before(doc,"first",7,out _));
        doc=Fixture("first"); doc["effects"]![1]!["name"]="budget_red"; Assert.False(VfxBudgetProof.Before(doc,"first",7,out _));
        doc=Fixture("first"); doc["effects"]![0]!["definition"]=Guid.NewGuid().ToString(); Assert.False(VfxBudgetProof.Before(doc,"first",7,out _));
    }
    [Fact] public void After_requires_evidence_of_a_different_process_and_exactly_three_more_slots()
    {
        var first=Fixture("first"); Assert.True(VfxBudgetProof.Before(first,"first",7,out var evidence));
        var second=(JsonObject)first.DeepClone(); second["process"]="second";
        Assert.True(VfxBudgetProof.After(evidence,second,"second",10));
        Assert.False(VfxBudgetProof.After(null,second,"second",10));
        Assert.False(VfxBudgetProof.After(evidence,first,"first",10));
        Assert.False(VfxBudgetProof.After(evidence,second,"second",11));
        Assert.False(VfxBudgetProof.After(evidence,second,"second",9));
        Assert.False(VfxBudgetProof.After(evidence,first,"second",10));
        second["effects"]![0]!["registrationId"]=Guid.NewGuid().ToString(); Assert.False(VfxBudgetProof.After(evidence,second,"second",10));
    }
}
