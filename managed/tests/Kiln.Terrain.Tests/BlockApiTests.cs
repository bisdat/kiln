// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.TestKit;
using Xunit;
namespace TerrainTests;
[Collection(nameof(BuildGuardTests))]
public sealed class BlockApiTests
{
    [Fact]
    public void OldTableRefusesNewRowsWithoutReadingPastTheHeader()
    {
        FakeBuildTable.Use();using var kit=new KilnHarness();kit.Step(2);
        Assert.Equal(BlockRefusal.ServiceOlder,Build.BlockAt(new WorldPosition(1,2,3)).Refusal);
        Assert.Equal(BlockRefusal.ServiceOlder,Build.FindWall(new WorldPosition(1,2,3),new WorldPosition(2,2,3)).Refusal);
        Assert.Equal(BlockRefusal.ServiceOlder,Build.BreakBox(new CellBox(0,0,0,1,1,1)).Refusal);
    }
    [Fact]
    public void QueryNotFoundIsNotARefusal()
    {
        var r=new BlockQueryResult(null,BlockRefusal.None);
        Assert.True(r.Ok);Assert.False(r.Found);Assert.Null(r.Reason);
    }
    [Fact]
    public void FaultResultNeverPromisesRollback()
    {
        var r=new BlockBreakResult(BlockRefusal.Faulted,0,1,1,0,0,0,0,0,true,false,1,false);
        Assert.False(r.Ok);Assert.True(r.MayHaveChanged);Assert.Equal(1UL,r.Removed);
        Assert.Contains("restart",r.Reason!,StringComparison.OrdinalIgnoreCase);
    }
    [Fact]
    public void OptionsDefaultToDamageNotUnrestrictedInstantEditing()
    {
        var o=new BlockBreakOptions();Assert.Equal(BlockBreakFlags.None,o.Flags);
        Assert.Empty(o.Damage);Assert.Empty(o.Materials);Assert.Null(o.Selection);
    }
}
