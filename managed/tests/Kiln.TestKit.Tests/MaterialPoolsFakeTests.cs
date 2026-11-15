#pragma warning disable KILNX028
using Xunit;
namespace Kiln.TestKit.Tests;
public sealed class MaterialPoolsFakeTests
{
    private sealed class PoolMod:Mod;
    private static BuildingMaterialPoolBlock Pool(int n=2)=>new() {Name="Walls",Materials=Enumerable.Range(0,n).Select(i=>new BuildingMaterialPoolEntry(GameMaterial:i%2==0?"WoodRough":"StoneRough")).ToArray()};
    [Fact]
    public void An_unloaded_owner_retains_the_immutable_row_without_reporting_it_registered()
    {
        using var h=new KilnHarness();h.WorldTables.ServePool("pool-unload","walls",Pool());
        var mod=h.Load<PoolMod>(new ModManifest("pool-unload").Set("clients","mod"));h.Step(2);
        var r=h.As(mod,()=>Blocks.Define("walls",Pool(),new DefinitionRegistrationOptions()));
        h.Unload(mod);h.Step(2);
        var retained=h.Run(()=>WorldTables.GetMaterialPoolRegistration(r.RegistrationId));
        Assert.Equal(DefinitionRegistrationState.Retained,retained.State);Assert.Equal(2u,retained.SlotsConsumed);
        var b=h.Run(()=>WorldTables.GetPoolBudget("pool-unload:walls")).Budget;
        Assert.Equal(0u,b.KilnUsed);Assert.Equal(2u,b.Reserved);
    }
    [Fact]
    public void Served_pool_is_registered_with_its_actual_entry_count_and_budget()
    {
        using var h=new KilnHarness();h.WorldTables.ServePool("pool-kit","walls",Pool());
        var mod=h.Load<PoolMod>(new ModManifest("pool-kit").Set("clients","mod"));h.Step(2);
        var r=h.As(mod,()=>Blocks.Define("walls",Pool(),new DefinitionRegistrationOptions()));
        Assert.True(r.Ok,r.Reason);Assert.Equal(DefinitionRegistrationState.Registered,r.State);Assert.Equal(2u,r.SlotsConsumed);
        Assert.Equal(2u,h.Run(()=>WorldTables.GetPoolBudget("pool-kit:walls")).Budget.KilnUsed);
        Assert.Equal(0u,h.Run(()=>WorldTables.GetPoolBudget("pool-kit:walls")).Budget.GameUsed);
        Assert.Equal(r.RegistrationId,h.As(mod,()=>Blocks.Define("walls",Pool(),new DefinitionRegistrationOptions())).RegistrationId);
        Assert.Equal(79u,h.Run(()=>WorldTables.GetMaterialBudget(WorldMaterialCategory.Building)).Budget.GameUsed);
        Assert.Equal(CapacityReadiness.NotFound,h.Run(()=>WorldTables.GetPoolBudget("pool-kit:unknown")).Budget.Readiness);
    }
    [Fact]
    public void Late_pool_require_refuses_allow_persists_once_without_charging_live_entries()
    {
        using var h=new KilnHarness();var mod=h.Load<PoolMod>(new ModManifest("pool-late").Set("clients","mod"));h.Step(2);
        var refused=h.As(mod,()=>Blocks.Define("new",Pool(),new DefinitionRegistrationOptions()));
        Assert.False(refused.Ok);Assert.Equal(DefinitionRefusalCode.RegistryClosed,refused.Code);Assert.Equal(0,h.WorldTables.PendingCount);
        var accepted=h.As(mod,()=>Blocks.Define("new",Pool(),new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart)));
        Assert.True(accepted.Ok,accepted.Reason);Assert.Equal(DefinitionRegistrationState.AcceptedNextStart,accepted.State);Assert.Equal(0u,accepted.SlotsConsumed);
        Assert.Equal(1,h.WorldTables.PendingCount);
        var again=h.As(mod,()=>Blocks.Define("new",Pool(),new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart)));
        Assert.Equal(accepted.SnapshotRevision,again.SnapshotRevision);Assert.Equal(1,h.WorldTables.PendingCount);
        Assert.Equal(accepted.State,h.Run(()=>WorldTables.GetMaterialPoolRegistration(accepted.RegistrationId)).State);
        Assert.False(h.Run(()=>WorldTables.GetPoolBudget("pool-late:new")).Ok);
    }
    [Fact]
    public void Refusals_and_bad_arguments_match_native_admission()
    {
        using var h=new KilnHarness();var mod=h.Load<PoolMod>(new ModManifest("pool-errors").Set("clients","mod"));h.Step(2);
        var missing=Pool() with {Materials=[new(GameMaterial:"Missing")]};
        Assert.Equal(DefinitionRefusalCode.UnknownBase,h.As(mod,()=>Blocks.Define("missing",missing,new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart))).Code);
        Assert.Throws<ArgumentException>(()=>h.As(mod,()=>Blocks.Define("duplicate",Pool(3),new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart))));
        Assert.Throws<ArgumentException>(()=>h.As(mod,()=>Blocks.Define("roof",Pool() with {Roof=true},new DefinitionRegistrationOptions())));
        Assert.Throws<ArgumentException>(()=>h.As(mod,()=>Blocks.Define("too-many",Pool(65),new DefinitionRegistrationOptions())));
        h.WorldTables.StorageFails=true;
        Assert.Equal(DefinitionRefusalCode.StorageFailure,h.As(mod,()=>Blocks.Define("storage",Pool(),new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart))).Code);
        Assert.Equal(0,h.WorldTables.PendingCount);
    }
    [Fact]
    public void Change_notice_waits_for_post_and_pending_write_does_not_increase_live_capacity()
    {
        using var h=new KilnHarness();var mod=h.Load<PoolMod>(new ModManifest("pool-events").Set("clients","mod"));h.Step(2);
        h.Run(()=>WorldTables.GetMaterialBudget(WorldMaterialCategory.Building));h.Step();
        var definitions=new List<DefinitionRegistrationChangedEvent>();var capacities=new List<CapacityChangedEvent>();
        h.As(mod,()=>{Events.On<DefinitionRegistrationChangedEvent>(definitions.Add);Events.On<CapacityChangedEvent>(capacities.Add);});
        var r=h.As(mod,()=>Blocks.Define("later",Pool(),new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart)));
        Assert.Empty(definitions);Assert.Empty(capacities);h.Step();
        Assert.Contains(definitions,e=>e.Current.RegistrationId==r.RegistrationId);Assert.Empty(capacities);
    }
}
