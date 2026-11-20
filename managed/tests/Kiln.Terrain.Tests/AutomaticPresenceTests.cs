// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Nodes;
using Kiln;
using Xunit;
namespace TerrainTests;
public sealed class AutomaticPresenceTests
{
    private static readonly CellBox Box=new(64,104,64,8,8,8);
    private static TerrainPlan Plan(string id="cloud") => TerrainPlan.Lower(new TerrainPatch(id,1,Box,
        new TerrainVoxelVolume(Enumerable.Repeat((byte)1,512).ToArray()),[new(12)]));
    private static MemoryWorld OldWorld() => new() { ProvenNewSession=false, Match=DataMatch.None };
    private static void Seed(MemoryWorld w,int count)
    {
        for(var i=0;i<count;i++) w.Set(Box.X+i%8,Box.Y+(i/8)%8,Box.Z+i/64,Voxel.Of(12));
    }
    private static double Share(object result) => Assert.IsType<double>(result.GetType().GetProperty("PresenceShare")?.GetValue(result));
    [Theory]
    [InlineData(0,"Completed")]
    [InlineData(512,"Adopted")]
    [InlineData(341,"NeedsAdoption")]
    [InlineData(51,"Completed")]
    [InlineData(128,"NeedsAdoption")]
    [InlineData(127,"Completed")]
    public async Task Automatic_measures_existing_terrain_before_its_first_write(int equal,string state)
    {
        var w=OldWorld(); Seed(w,equal);
        var b=w.For("m");
        var e=new TerrainEngine(b,new TerrainSession(),null);
        var r=await e.Apply(Plan(),new(),8);
        Assert.Equal(state,r.State.ToString());
        Assert.Equal(equal/512d,Share(r));
        Assert.Contains("75%",r.Reason);
        if(state=="Completed")
        {
            Assert.NotEmpty(w.Writes);
            Assert.True((bool)r.GetType().GetProperty("InstalledWithoutProof")!.GetValue(r)!);
            var json=JsonSerializer.Serialize(b.Book());
            Assert.Contains("InstalledWithoutProof",json);
            Assert.Equal(equal/512d,Share(e.Applied().Single()));
        }
        else
        {
            Assert.Empty(w.Writes);
            if(state=="Adopted") {Assert.Contains("already present",r.Reason); Assert.True(e.Applied().Single().Adopted);}
            else {Assert.Contains("25%",r.Reason); Assert.Empty(e.Applied());}
        }
    }
    [Fact]
    public async Task Legacy_ledger_with_another_plan_is_history_and_stays_legacy_after_serialization()
    {
        var w=new MemoryWorld(); var b=w.For("m");
        var e=new TerrainEngine(b,new TerrainSession(),null);
        await e.Apply(Plan("earlier"),new(),8);
        var old=JsonNode.Parse(JsonSerializer.Serialize(b.Book()))!.AsObject();
        old.Remove("EstablishedInNewWorld");
        b.Save(JsonSerializer.Deserialize<TerrainBook>(old.ToJsonString())!);
        w.ProvenNewSession=false; w.Match=DataMatch.Exact;
        w.Edits.Clear(); Seed(w,341);
        var r=await new TerrainEngine(b,new TerrainSession(),null).Apply(Plan("new-plan"),new(),8);
        Assert.Equal(TerrainState.Completed,r.State);
        Assert.Equal(171,r.ChangedCells);
        Assert.Null(r.GetType().GetProperty("PresenceShare")?.GetValue(r));
        Assert.DoesNotContain("EstablishedInNewWorld",JsonSerializer.Serialize(b.Book()));
    }
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task A_new_unproven_ledger_still_samples_in_a_later_session(bool firstInstalled)
    {
        var w=OldWorld(); if(!firstInstalled) Seed(w,341); var b=w.For("m");
        var first=await new TerrainEngine(b,new TerrainSession(),null).Apply(Plan("earlier"),new(),8);
        Assert.Equal(firstInstalled ? TerrainState.Completed : TerrainState.NeedsAdoption,first.State);
        var json=JsonSerializer.Serialize(b.Book());
        Assert.Contains("\"EstablishedInNewWorld\":false",json);
        b.Save(JsonSerializer.Deserialize<TerrainBook>(json)!);
        w.Edits.Clear(); w.Writes.Clear(); Seed(w,341);
        var later=await new TerrainEngine(b,new TerrainSession(),null).Apply(Plan(),new(),8);
        Assert.Equal(TerrainState.NeedsAdoption,later.State);
        Assert.Equal(341/512d,Share(later)); Assert.Empty(w.Writes);
    }
    [Theory]
    [InlineData(0,TerrainState.Completed)]
    [InlineData(341,TerrainState.NeedsAdoption)]
    [InlineData(512,TerrainState.Adopted)]
    public async Task Inspect_reports_the_measurement_and_its_limit_without_installing(int equal,TerrainState state)
    {
        var w=OldWorld(); Seed(w,equal);
        var r=await new TerrainEngine(w.For("m"),new TerrainSession(),null).Inspect(Plan(),new(),8);
        Assert.Equal(state,r.Would); Assert.Equal(equal/512d,Share(r));
        Assert.Contains("75%",r.Reason);
        Assert.DoesNotContain(w.Writes,x=>!x.DryRun); Assert.Empty(w.For("m").Book().Plans);
    }
    [Fact]
    public async Task A_sampled_install_with_conflicts_still_reports_the_heuristic_limit()
    {
        var w=OldWorld(); w.Bases.Add(Box);
        var r=await new TerrainEngine(w.For("m"),new TerrainSession(),null).Apply(Plan(),new(),8);
        Assert.Equal(TerrainState.Conflict,r.State);
        Assert.Equal(0d,Share(r)); Assert.Contains("75%",r.Reason);
        Assert.Contains(r.Conflicts,c=>c.Kind==TerrainConflictKind.BaseZone);
    }
    [Fact]
    public async Task Leave_padding_is_not_presence_evidence()
    {
        var w=OldWorld();
        var source=new byte[512]; Array.Fill(source,(byte)1,256,256);
        for(var i=256;i<312;i++) w.Set(Box.X+i%8,Box.Y+(i/8)%8,Box.Z+i/64,Voxel.Of(12));
        var p=TerrainPlan.Lower(new TerrainPatch("pad",1,Box,new TerrainVoxelVolume(source),[new(12)]));
        var r=await new TerrainEngine(w.For("m"),new TerrainSession(),null).Apply(p,new(),8);
        Assert.Equal(TerrainState.Completed,r.State);
        Assert.Equal(56/256d,Share(r));
    }
    [Fact]
    public async Task Sampling_uses_the_shared_budget_and_yields_before_any_write()
    {
        var w=OldWorld(); var d=new BudgetTests.Driver {ReadCost=3};
        var p=TerrainPlan.Lower(new TerrainPatch("large",1,new(64,104,64,32,16,32),
            new TerrainVoxelVolume(Enumerable.Repeat((byte)1,16384).ToArray()),[new(12)]));
        w.AfterWrite=()=>Assert.True(d.Reads.Values.Sum()>=p.AddressedCells);
        var e=new TerrainEngine(new BudgetTests.Scheduled(w.For("m"),d,false),new TerrainSession(),null);
        var t=e.Apply(p,new(),32);
        Assert.False(t.IsCompleted); Assert.Empty(w.Writes);
        d.Finish(t); Assert.Equal(TerrainState.Completed,(await t).State);
        Assert.Equal(0d,Share(await t));
        Assert.All(d.Reads.Values,n=>Assert.InRange(n,0,32768));
        Assert.All(d.Writes.Values,n=>Assert.InRange(n,0,4096));
    }
}
