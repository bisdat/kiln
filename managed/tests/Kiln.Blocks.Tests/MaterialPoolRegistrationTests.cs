#pragma warning disable KILNX028
using System.Runtime.InteropServices;
using Xunit;
namespace Kiln.Blocks.Tests;
[Collection("item icons")]
public sealed class MaterialPoolRegistrationTests
{
    [Fact]
    public void Abi_appends_preserve_the_shipped_prefix()
    {
        Assert.Equal(176,Marshal.SizeOf<WorldTablesNative.Table>());
        Assert.Equal(144,Marshal.OffsetOf<WorldTablesNative.Table>(nameof(WorldTablesNative.Table.MaterialBudget)).ToInt32());
        Assert.Equal(152,Marshal.OffsetOf<WorldTablesNative.Table>(nameof(WorldTablesNative.Table.PoolBudget)).ToInt32());
        Assert.Equal(160,Marshal.OffsetOf<WorldTablesNative.Table>(nameof(WorldTablesNative.Table.DefinePoolResult)).ToInt32());
        Assert.Equal(168,Marshal.OffsetOf<WorldTablesNative.Table>(nameof(WorldTablesNative.Table.PoolRegistration)).ToInt32());
        Assert.True(WorldTablesNative.HasAppend(144,136));
        Assert.False(WorldTablesNative.HasAppend(144,144));
        Assert.True(WorldTablesNative.HasAppend(152,144));
        Assert.False(WorldTablesNative.HasAppend(152,152));
    }
    [Fact]
    public void Pool_queries_accept_existing_block_name_rules_without_an_extra_length_budget()
    {
        Assert.True(WorldTables.PoolName("mod:"+new string('a',200)));
        Assert.True(WorldTables.PoolName("mod:."));
    }
    [Fact]
    public void Malformed_queries_throw_before_the_bridge()
    {
        Assert.Throws<ArgumentOutOfRangeException>(()=>WorldTables.GetMaterialBudget((WorldMaterialCategory)99));
        Assert.Throws<ArgumentException>(()=>WorldTables.GetPoolBudget("bad pool"));
        Assert.Throws<ArgumentException>(()=>WorldTables.GetPoolBudget("a:b:c"));
        Assert.Throws<ArgumentException>(()=>WorldTables.GetMaterialPoolRegistration(Guid.Empty));
    }
    [Fact]
    public void Missing_appends_name_the_capability()
    {
        Assert.Equal("worldtables.budgets",Assert.Throws<KilnUnavailableException>(()=>WorldTables.GetMaterialBudget(WorldMaterialCategory.Building)).Capability);
        Assert.Equal("worldtables.material_pools",Assert.Throws<KilnUnavailableException>(()=>WorldTables.GetMaterialPoolRegistration(Guid.NewGuid())).Capability);
    }
    [Fact]
    public void Explicit_options_validate_before_native_or_pending_storage()
    {
        var mod=new ModBlocks("pools",1){ManifestClients="mod"};
        var block=new BuildingMaterialPoolBlock{Name="Pool",Materials=[new(GameMaterial:"WoodRough")]};
        Assert.Throws<ArgumentNullException>(()=>Compiler.DefinePoolResult(mod,"p",block,null!));
        Assert.Throws<ArgumentOutOfRangeException>(()=>Compiler.DefinePoolResult(mod,"p",block,new((DefinitionTiming)99)));
        Assert.Throws<ArgumentException>(()=>Compiler.DefinePoolResult(mod,"p",block with {Materials=[]},new()));
    }
}
