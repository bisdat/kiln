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
        kit.Run(() =>
        {
            Assert.Equal(BlockRefusal.ServiceOlder,Build.BlockAt(new WorldPosition(1,2,3)).Refusal);
            Assert.Equal(BlockRefusal.ServiceOlder,Build.FindWall(new WorldPosition(1,2,3),new WorldPosition(2,2,3)).Refusal);
            Assert.Equal(BlockRefusal.ServiceOlder,Build.BreakBox(new CellBox(0,0,0,1,1,1)).Refusal);
        });
    }
    [Fact]
    public void NewRowsMarshalTypedQueriesAndCallerDamageAndSpherePolicies()
    {
        FakeBuildTable.BlockRows(true);
        try
        {
            using var kit = new KilnHarness(); kit.Step(2);
            kit.Run(() =>
            {
                var q = Build.BlockAt(new WorldPosition(0,0,0));
                Assert.True(q.Ok); Assert.Equal(BuildBlockKind.Building,q.Info!.Kind); Assert.Equal((byte)143,q.Info.Material);
                Assert.Equal(BlockHardness.ModeratelyHard,q.Info.Hardness); Assert.Equal(500,q.Info.Health);
                Assert.Equal(7,q.Info.Damage); Assert.Equal(BlockProvenance.PlayerPlaced,q.Info.Provenance);
                Assert.True(Build.FindWall(new WorldPosition(0,0,0),new WorldPosition(1,1,1),FindWallFlags.PlayerTerrain).Found);
                Assert.Equal(1u,FakeBuildTable.FindFlags);
                var r = Build.BreakBox(new CellBox(0,0,0,1,1,1),new BlockBreakOptions
                {
                    Flags = BlockBreakFlags.PlayerTerrain, TerrainHealthScale = 250,
                    TerrainHealAfter = TimeSpan.FromSeconds(20), Selection = [true],
                    ProtectedSphere = new(new WorldPosition(0,0,0),1,.01,.25), Build = new() { Journal = false },
                });
                Assert.True(r.Ok); Assert.True(r.MayHaveChanged); Assert.True(r.Verified); Assert.Equal(1UL,r.Removed);
                Assert.Equal(184u,FakeBuildTable.BlockRequestSize); Assert.Equal(24u,FakeBuildTable.BlockFlags);
                Assert.Equal(250,FakeBuildTable.TerrainScale); Assert.Equal(20,FakeBuildTable.TerrainExpiry);
                Assert.Equal(1,FakeBuildTable.SphereRadius); Assert.Equal(.01,FakeBuildTable.SphereMargin);
                Assert.Equal(.25,FakeBuildTable.SphereMinimum); Assert.Equal((byte)1,FakeBuildTable.SelectionFirst);
            });
        }
        finally { FakeBuildTable.BlockRows(false); }
    }
    [Theory]
    [InlineData(0u, true, true)]
    [InlineData(22u, false, false)]
    [InlineData(22u, true, false)]
    public void EditEventVerificationDoesNotUpgradeCountReadbackToExactSuccess(uint refusal, bool writerVerified, bool expected)
    {
        FakeBuildTable.BlockRows(true);
        FakeBuildTable.BreakRefusalCode = refusal; FakeBuildTable.BreakWriterVerified = writerVerified;
        try
        {
            using var kit = new KilnHarness(); kit.Step(2);
            kit.Run(() =>
            {
                BuildEditEvent? observed = null;
                using var subscription = Events.On<BuildEditEvent>(e => observed = e);
                var r = Build.BreakBox(new CellBox(0,0,0,1,1,1),new BlockBreakOptions { Build = new() { Journal = false } });
                Assert.True(r.Verified); // this flag verifies the observed removal count, not the whole edit
                Assert.NotNull(observed?.Result);
                Assert.Equal(expected,observed!.Result!.Verified);
                Assert.Equal(1L,observed.Result.CellsChanged);
            });
        }
        finally { FakeBuildTable.BlockRows(false); }
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
